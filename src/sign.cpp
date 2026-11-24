#include <sc/pdflite/sign.h>

#include <sc/pdflite/encryption.h>
#include <sc/pdflite/file.h>
#include <sc/pdflite/object.h>
#include <sc/pdflite/reader.h>

#include <openssl/bio.h>
#include <openssl/cms.h>
#include <openssl/err.h>
#include <openssl/ess.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/opensslv.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/pkcs7.h>
#include <openssl/rand.h>
#include <openssl/ts.h>
#include <openssl/x509.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <utility>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>

namespace sc::pdflite {
    namespace {
        // OpenSSL's last error, for a message.
        std::string openssl_error() {
            const unsigned long code = ERR_get_error();
            ERR_clear_error();
            if (code == 0) return "OpenSSL failed";
            char text[256];
            ERR_error_string_n(code, text, sizeof text);
            return text;
        }

        [[noreturn]] void fail(const std::string &what) { throw std::runtime_error("sc::pdflite: " + what + ": " + openssl_error()); }

        template<typename T, void (*Free)(T *)>
        struct owned {
            T *pointer = nullptr;
            owned() = default;
            explicit owned(T *p) : pointer(p) {}
            owned(const owned &) = delete;
            owned &operator=(const owned &) = delete;
            owned(owned &&other) noexcept : pointer(std::exchange(other.pointer, nullptr)) {}
            owned &operator=(owned &&other) noexcept {
                if (this != &other) {
                    if (pointer) Free(pointer);
                    pointer = std::exchange(other.pointer, nullptr);
                }
                return *this;
            }
            ~owned() { if (pointer) Free(pointer); }
            T *get() const { return pointer; }
            T *release() { return std::exchange(pointer, nullptr); }
            explicit operator bool() const { return pointer != nullptr; }
        };

        using bio = owned<BIO, BIO_free_all>;
        using cms_info = owned<CMS_ContentInfo, CMS_ContentInfo_free>;

        bio memory(const std::string_view bytes) {
            bio in{BIO_new_mem_buf(bytes.data(), static_cast<int>(bytes.size()))};
            if (!in) fail("can't read memory");
            return in;
        }

        template<typename T, typename Encode>
        std::string der_of(T *value, Encode encode) {
            const int size = encode(value, nullptr);
            if (size <= 0) fail("can't encode");
            std::string out(static_cast<std::size_t>(size), '\0');
            auto *at = reinterpret_cast<unsigned char *>(out.data());
            encode(value, &at);
            return out;
        }

        // An ASN.1 string's length (ASN1_STRING_length() is deprecated from OpenSSL 4.1).
        std::size_t length_of(const ASN1_STRING *value) {
#if OPENSSL_VERSION_NUMBER >= 0x40100000L
            return static_cast<std::size_t>(ASN1_STRING_get_length(value));
#else
            return static_cast<std::size_t>(ASN1_STRING_length(value));
#endif
        }

        std::string sha256(const std::string_view data) {
            unsigned char digest[EVP_MAX_MD_SIZE];
            unsigned int size = 0;
            if (!EVP_Digest(data.data(), data.size(), digest, &size, EVP_sha256(), nullptr)) fail("SHA-256");
            return {reinterpret_cast<const char *>(digest), size};
        }

        std::string name_of(const X509 *certificate) {
            bio out{BIO_new(BIO_s_mem())};
            X509_NAME_print_ex(out.get(), X509_get_subject_name(certificate), 0, XN_FLAG_RFC2253 & ~ASN1_STRFLGS_ESC_MSB);
            char *text = nullptr;
            const long size = BIO_get_mem_data(out.get(), &text);
            return {text, static_cast<std::size_t>(size)};
        }

        std::string common_name(const X509 *certificate) {
            const X509_NAME *subject = X509_get_subject_name(certificate);
            const int at = X509_NAME_get_index_by_NID(subject, NID_commonName, -1);
            if (at < 0) return {};
            const ASN1_STRING *value = X509_NAME_ENTRY_get_data(X509_NAME_get_entry(subject, at));
            unsigned char *utf8 = nullptr;
            const int size = ASN1_STRING_to_UTF8(&utf8, value);
            if (size < 0) return {};
            std::string out{reinterpret_cast<const char *>(utf8), static_cast<std::size_t>(size)};
            OPENSSL_free(utf8);
            return out;
        }

        // The bytes of a hex string, ignoring white space; stops at anything else.
        std::string from_hex(const std::string_view hex) {
            std::string out;
            int high = -1;
            for (const char c: hex) {
                int value;
                if (c >= '0' && c <= '9') value = c - '0';
                else if (c >= 'a' && c <= 'f') value = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') value = c - 'A' + 10;
                else if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
                else break;
                if (high < 0) high = value;
                else {
                    out += static_cast<char>(high << 4 | value);
                    high = -1;
                }
            }
            return out;
        }

        std::string to_hex(const std::string_view bytes) {
            static constexpr char digits[] = "0123456789ABCDEF";
            std::string out;
            out.reserve(bytes.size() * 2);
            for (const char c: bytes) {
                const auto byte = static_cast<unsigned char>(c);
                out += digits[byte >> 4];
                out += digits[byte & 15];
            }
            return out;
        }

        // value with its strings encrypted with a file's key (an R5/R6 file's key is every object's).
        object sealed(const object &value, const std::optional<std::string> &key) {
            if (!key) return value;
            if (value.is<string>()) return string{encryption::encrypt(*key, value.as<string>().bytes()), true};
            if (value.is<array>()) {
                array out;
                for (const auto &item: value.as<array>()) out.push_back(sealed(item, key));
                return out;
            }
            if (value.is<dictionary>()) {
                dictionary out;
                for (const auto &[entry, item]: value.as<dictionary>()) out.add(entry.text(), sealed(item, key));
                return out;
            }
            return value;
        }

        // The offset after the last "startxref", and whether the section there is a table.
        std::pair<std::uint64_t, bool> last_section(const std::string_view pdf) {
            const auto keyword = pdf.rfind("startxref");
            if (keyword == std::string_view::npos) throw std::invalid_argument("sc::pdflite: no startxref: not a PDF, or cut short");
            std::size_t at = keyword + 9;
            while (at < pdf.size() && std::isspace(static_cast<unsigned char>(pdf[at]))) ++at;
            std::uint64_t offset = 0;
            while (at < pdf.size() && std::isdigit(static_cast<unsigned char>(pdf[at]))) offset = offset * 10 + static_cast<std::uint64_t>(pdf[at++] - '0');
            if (offset >= pdf.size()) throw std::invalid_argument("sc::pdflite: startxref points past the end of the file");
            std::size_t start = offset;
            while (start < pdf.size() && std::isspace(static_cast<unsigned char>(pdf[start]))) ++start;
            return {offset, pdf.substr(start, 4) == "xref"};
        }

        // The ids of an AcroForm's fields, and their names, looking into /Kids.
        void field_names(const reader &in, const object &fields, std::set<std::string> &names, int depth = 0) {
            const object list = in.resolve(fields);
            if (!list.is<array>() || depth > 8) return;
            for (const auto &item: list.as<array>()) {
                const object field = in.resolve(item);
                if (!field.is<dictionary>()) continue;
                if (const auto *t = field.as<dictionary>().find("T"); t && t->is<string>()) names.insert(t->as<string>().bytes());
                if (const auto *kids = field.as<dictionary>().find("Kids")) field_names(in, *kids, names, depth + 1);
            }
        }

#ifndef CMS_NO_SIGNING_TIME
        // A signer's attribute set (SET OF Attribute) in DER, as the signature covers it: each
        // attribute's encoding, sorted (X.690's order for a SET OF).
        std::string signed_attributes(CMS_SignerInfo *signer) {
            std::vector<std::string> encodings;
            for (int i = 0; i < CMS_signed_get_attr_count(signer); ++i) encodings.push_back(der_of(CMS_signed_get_attr(signer, i), i2d_X509_ATTRIBUTE));
            std::ranges::sort(encodings);
            std::string content;
            for (const auto &each: encodings) content += each;
            std::string out(1, '\x31');
            if (content.size() < 128) {
                out += static_cast<char>(content.size());
            } else {
                std::string length;
                for (std::size_t size = content.size(); size; size >>= 8) length.insert(length.begin(), static_cast<char>(size & 0xFF));
                out += static_cast<char>(0x80 | length.size());
                out += length;
            }
            return out + content;
        }
#endif

        // The CMS SignedData (detached) of content: PAdES B-B, and B-T when there's an authority.
        std::string signed_data(const std::string_view content, const signing_key::impl &key, const signature_options &options);
    }

    struct signing_key::impl {
        owned<X509, X509_free> certificate;
        owned<EVP_PKEY, EVP_PKEY_free> key;
        STACK_OF(X509) *chain = nullptr;

        impl() = default;
        impl(const impl &) = delete;
        impl &operator=(const impl &) = delete;
        ~impl() { if (chain) sk_X509_pop_free(chain, X509_free); }

        void check() const {
            if (!certificate || !key) throw std::invalid_argument("sc::pdflite: a signing key needs a certificate and its private key");
            if (X509_check_private_key(certificate.get(), key.get()) != 1) {
                ERR_clear_error();
                throw std::invalid_argument("sc::pdflite: the private key isn't the certificate's");
            }
        }
    };

    namespace {
        void read_chain(const std::string_view pem, STACK_OF(X509) *chain) {
            if (pem.empty()) return;
            bio in = memory(pem);
            while (X509 *certificate = PEM_read_bio_X509(in.get(), nullptr, nullptr, nullptr)) sk_X509_push(chain, certificate);
            ERR_clear_error(); // the end of the text
        }

        std::string signed_data(const std::string_view content, const signing_key::impl &key, const signature_options &options) {
            bio data = memory(content);
            int flags = CMS_DETACHED | CMS_BINARY | CMS_PARTIAL | CMS_NOSMIMECAP | CMS_CADES;
#ifdef CMS_NO_SIGNING_TIME
            flags |= CMS_NO_SIGNING_TIME;
#endif
            cms_info cms{CMS_sign(nullptr, nullptr, key.chain, nullptr, flags)};
            if (!cms) fail("can't start a CMS signature");
            CMS_SignerInfo *signer = CMS_add1_signer(cms.get(), key.certificate.get(), key.key.get(), EVP_sha256(), flags);
            if (!signer) fail("can't add the signer");
            if (CMS_final(cms.get(), data.get(), nullptr, flags) != 1) fail("can't sign");
#ifndef CMS_NO_SIGNING_TIME
            // OpenSSL before 3.2 always adds a signing time, which PAdES forbids (the claimed time is
            // the signature dictionary's /M): taken out, and the attributes signed again.
            if (const int at = CMS_signed_get_attr_by_NID(signer, NID_pkcs9_signingTime, -1); at >= 0) {
                X509_ATTRIBUTE_free(CMS_signed_delete_attr(signer, at));
                const std::string attributes = signed_attributes(signer);
                owned<EVP_MD_CTX, EVP_MD_CTX_free> context{EVP_MD_CTX_new()};
                std::size_t size = 0;
                if (EVP_DigestSignInit(context.get(), nullptr, EVP_sha256(), nullptr, key.key.get()) != 1 ||
                    EVP_DigestSign(context.get(), nullptr, &size, reinterpret_cast<const unsigned char *>(attributes.data()), attributes.size()) != 1)
                    fail("can't sign the attributes");
                std::string value(size, '\0');
                if (EVP_DigestSign(context.get(), reinterpret_cast<unsigned char *>(value.data()), &size,
                                   reinterpret_cast<const unsigned char *>(attributes.data()), attributes.size()) != 1)
                    fail("can't sign the attributes");
                value.resize(size);
                if (ASN1_STRING_set(CMS_SignerInfo_get0_signature(signer), value.data(), static_cast<int>(value.size())) != 1) fail("can't set the signature");
            }
#endif
            if (options.timestamp) {
                // RFC 3161: the authority signs the signature value's digest and its time (B-T).
                const ASN1_OCTET_STRING *value = CMS_SignerInfo_get0_signature(signer);
                const std::string imprint = sha256({reinterpret_cast<const char *>(ASN1_STRING_get0_data(value)), static_cast<std::size_t>(length_of(value))});
                owned<TS_REQ, TS_REQ_free> request{TS_REQ_new()};
                owned<TS_MSG_IMPRINT, TS_MSG_IMPRINT_free> message{TS_MSG_IMPRINT_new()};
                owned<X509_ALGOR, X509_ALGOR_free> algorithm{X509_ALGOR_new()};
                X509_ALGOR_set_md(algorithm.get(), EVP_sha256());
                owned<ASN1_INTEGER, ASN1_INTEGER_free> nonce{ASN1_INTEGER_new()};
                unsigned char random[8];
                RAND_bytes(random, sizeof random);
                random[0] &= 0x7F;
                owned<BIGNUM, BN_free> number{BN_bin2bn(random, sizeof random, nullptr)};
                BN_to_ASN1_INTEGER(number.get(), nonce.get());
                if (!TS_REQ_set_version(request.get(), 1) || !TS_MSG_IMPRINT_set_algo(message.get(), algorithm.get()) ||
                    !TS_MSG_IMPRINT_set_msg(message.get(), reinterpret_cast<unsigned char *>(const_cast<char *>(imprint.data())), static_cast<int>(imprint.size())) ||
                    !TS_REQ_set_msg_imprint(request.get(), message.get()) || !TS_REQ_set_nonce(request.get(), nonce.get()) ||
                    !TS_REQ_set_cert_req(request.get(), 1))
                    fail("can't make a time-stamp request");
                const std::string answer = options.timestamp(der_of(request.get(), i2d_TS_REQ));
                const auto *at = reinterpret_cast<const unsigned char *>(answer.data());
                owned<TS_RESP, TS_RESP_free> response{d2i_TS_RESP(nullptr, &at, static_cast<long>(answer.size()))};
                if (!response) fail("the time-stamp authority's answer isn't a time-stamp response");
                const long status = ASN1_INTEGER_get(TS_STATUS_INFO_get0_status(TS_RESP_get_status_info(response.get())));
                if (status != 0 && status != 1) throw std::runtime_error("sc::pdflite: the time-stamp authority refused the request (status " + std::to_string(status) + ")");
                PKCS7 *token = TS_RESP_get_token(response.get());
                TS_TST_INFO *info = TS_RESP_get_tst_info(response.get());
                if (!token || !info) throw std::runtime_error("sc::pdflite: the time-stamp authority's answer holds no token");
                const ASN1_OCTET_STRING *digest = TS_MSG_IMPRINT_get_msg(TS_TST_INFO_get_msg_imprint(info));
                if (std::string_view{reinterpret_cast<const char *>(ASN1_STRING_get0_data(digest)), static_cast<std::size_t>(length_of(digest))} != imprint)
                    throw std::runtime_error("sc::pdflite: the time-stamp is for something else (its digest isn't the signature's)");
                if (const ASN1_INTEGER *returned = TS_TST_INFO_get_nonce(info); !returned || ASN1_INTEGER_cmp(returned, nonce.get()) != 0)
                    throw std::runtime_error("sc::pdflite: the time-stamp's nonce isn't the request's");
                const std::string token_der = der_of(token, i2d_PKCS7);
                if (CMS_unsigned_add1_attr_by_NID(signer, NID_id_smime_aa_timeStampToken, V_ASN1_SEQUENCE, token_der.data(), static_cast<int>(token_der.size())) != 1)
                    fail("can't add the time-stamp");
            }
            return der_of(cms.get(), i2d_CMS_ContentInfo);
        }

        // An upper bound on a signature's size: the certificates it carries, the signature value,
        // the attributes, and a time-stamp token (with the authority's certificates).
        std::size_t room_for(const signing_key::impl &key, const signature_options &options) {
            std::size_t size = 2048 + static_cast<std::size_t>(EVP_PKEY_get_size(key.key.get())) +
                               static_cast<std::size_t>(i2d_X509(key.certificate.get(), nullptr));
            for (int i = 0; key.chain && i < sk_X509_num(key.chain); ++i) size += static_cast<std::size_t>(i2d_X509(sk_X509_value(key.chain, i), nullptr));
            if (options.timestamp) size += 16384;
            return (size + 1023) / 1024 * 1024;
        }

        std::string padded(const std::uint64_t value) {
            char text[16];
            std::snprintf(text, sizeof text, "%010llu", static_cast<unsigned long long>(value));
            return text;
        }
    }

    signing_key signing_key::from_pem(const std::string_view certificate, const std::string_view key, const std::string_view passphrase,
                                      const std::string_view chain) {
        auto state = std::make_shared<impl>();
        state->chain = sk_X509_new_null();
        {
            bio in = memory(certificate);
            state->certificate = owned<X509, X509_free>{PEM_read_bio_X509(in.get(), nullptr, nullptr, nullptr)};
            if (!state->certificate) {
                ERR_clear_error();
                throw std::invalid_argument("sc::pdflite: the certificate isn't a PEM certificate");
            }
            while (X509 *more = PEM_read_bio_X509(in.get(), nullptr, nullptr, nullptr)) sk_X509_push(state->chain, more);
            ERR_clear_error();
        }
        read_chain(chain, state->chain);
        {
            bio in = memory(key);
            const std::string secret{passphrase};
            state->key = owned<EVP_PKEY, EVP_PKEY_free>{PEM_read_bio_PrivateKey(in.get(), nullptr, nullptr, const_cast<char *>(secret.c_str()))};
            if (!state->key) {
                ERR_clear_error();
                throw std::invalid_argument("sc::pdflite: the private key can't be read (not PEM, or a wrong passphrase)");
            }
        }
        state->check();
        return signing_key{std::move(state)};
    }

    signing_key signing_key::from_pkcs12(const std::string_view bytes, const std::string_view passphrase) {
        bio in = memory(bytes);
        owned<PKCS12, PKCS12_free> p12{d2i_PKCS12_bio(in.get(), nullptr)};
        if (!p12) {
            ERR_clear_error();
            throw std::invalid_argument("sc::pdflite: not a PKCS#12 file");
        }
        auto state = std::make_shared<impl>();
        EVP_PKEY *key = nullptr;
        X509 *certificate = nullptr;
        const std::string secret{passphrase};
        if (PKCS12_parse(p12.get(), secret.c_str(), &key, &certificate, &state->chain) != 1) {
            ERR_clear_error();
            throw std::invalid_argument("sc::pdflite: the PKCS#12 file can't be opened (a wrong passphrase?)");
        }
        state->key = owned<EVP_PKEY, EVP_PKEY_free>{key};
        state->certificate = owned<X509, X509_free>{certificate};
        if (!state->chain) state->chain = sk_X509_new_null();
        state->check();
        return signing_key{std::move(state)};
    }

    std::string signing_key::subject() const { return name_of(impl_->certificate.get()); }

    std::string sign(const std::string_view pdf, const signing_key &key, const signature_options &options) {
        std::optional<reader> opened;
        try {
            opened.emplace(reader::from_memory(std::string{pdf}, options.password));
        } catch (const std::exception &error) {
            throw std::invalid_argument(error.what());
        }
        const reader &in = *opened;
        const dictionary &trailer = in.trailer();
        const auto *root_entry = trailer.find("Root");
        if (!root_entry || !root_entry->is<reference>()) throw std::invalid_argument("sc::pdflite: the trailer's /Root isn't a reference");
        const std::uint32_t root = root_entry->as<reference>().id;
        if (in.pages().empty()) throw std::invalid_argument("sc::pdflite: a file without pages can't be signed");

        // An encrypted file's key, for the strings added to it.
        std::optional<std::string> file_key;
        if (const auto *encrypt = trailer.find("Encrypt")) {
            const object dictionary_value = in.resolve(*encrypt);
            if (!dictionary_value.is<dictionary>()) throw std::invalid_argument("sc::pdflite: the trailer's /Encrypt isn't a dictionary");
            try {
                file_key = encryption::key_for(dictionary_value.as<dictionary>(), options.password);
            } catch (const std::invalid_argument &) {
                throw std::invalid_argument("sc::pdflite: only files encrypted with AES-256 (revision 5 or 6) can be signed");
            }
            if (!file_key) throw std::invalid_argument("sc::pdflite: the password doesn't open this file");
        }

        std::uint32_t next = 1;
        if (const auto *size = trailer.find("Size"); size && size->is<std::int64_t>()) next = static_cast<std::uint32_t>(size->as<std::int64_t>());
        const std::uint32_t signature_id = next++, field_id = next++;
        const std::uint32_t page_id = in.pages().back();

        // The field's name: as asked, else the first free "SignatureN".
        std::set<std::string> taken;
        dictionary catalog = in.get(root).as<dictionary>();
        std::optional<std::uint32_t> form_id;
        dictionary form;
        if (const auto *found = catalog.find("AcroForm")) {
            if (found->is<reference>()) form_id = found->as<reference>().id;
            const object value = in.resolve(*found);
            if (value.is<dictionary>()) form = value.as<dictionary>();
            if (const auto *fields = form.find("Fields")) field_names(in, *fields, taken);
        }
        std::string field = options.field;
        if (field.empty() || taken.contains(field))
            for (int n = 1; field.empty() || taken.contains(field); ++n) field = (options.field.empty() ? "Signature" : options.field) + std::to_string(n);

        // The form lists the field, and says the file is signed (and to be updated by appending).
        array fields;
        if (const auto *existing = form.find("Fields"))
            if (const object list = in.resolve(*existing); list.is<array>())
                for (const auto &item: list.as<array>()) fields.push_back(item);
        fields.push_back(reference{field_id});
        form.set("Fields", fields);
        form.set("SigFlags", 3);
        if (!form_id) form_id = next++;
        catalog.set("AcroForm", reference{*form_id});

        // The page shows the (invisible) widget.
        dictionary page = in.get(page_id).as<dictionary>();
        array annotations;
        if (const auto *existing = page.find("Annots"))
            if (const object list = in.resolve(*existing); list.is<array>())
                for (const auto &item: list.as<array>()) annotations.push_back(item);
        annotations.push_back(reference{field_id});
        page.set("Annots", annotations);

        const dictionary widget{{"Type", name{"Annot"}}, {"Subtype", name{"Widget"}}, {"FT", name{"Sig"}}, {"T", string::text(field)},
                                {"V", reference{signature_id}}, {"F", 132}, {"Rect", array{0, 0, 0, 0}}, {"P", reference{page_id}}};

        const auto &state = *key.impl_;
        const std::size_t reserved = options.reserved ? options.reserved : room_for(state, options);
        dictionary details{{"M", string::date(std::chrono::system_clock::now())}};
        const std::string signer_name = options.name.empty() ? common_name(state.certificate.get()) : options.name;
        if (!signer_name.empty()) details.add("Name", string::text(signer_name));
        if (!options.reason.empty()) details.add("Reason", string::text(options.reason));
        if (!options.location.empty()) details.add("Location", string::text(options.location));
        if (!options.contact.empty()) details.add("ContactInfo", string::text(options.contact));
        details.add("Prop_Build", dictionary{{"App", dictionary{{"Name", name{"simply-cpp-pdflite"}}}}});

        // The update: what changed, appended after the file as it was.
        std::string out{pdf};
        if (!out.ends_with('\n')) out += '\n';
        std::map<std::uint32_t, std::uint64_t> offsets;
        const auto put = [&](const std::uint32_t id, const object &value) {
            offsets[id] = out.size();
            out += std::to_string(id) + " 0 obj\n" + sealed(value, file_key).write() + "\nendobj\n";
        };
        put(root, catalog);
        put(page_id, page);
        put(*form_id, form);
        put(field_id, widget);
        // The signature dictionary: /Contents isn't encrypted (7.6.2), /ByteRange is filled in last.
        offsets[signature_id] = out.size();
        out += std::to_string(signature_id) + " 0 obj\n<</Type /Sig /Filter /Adobe.PPKLite /SubFilter /ETSI.CAdES.detached";
        for (const auto &[entry, value]: details) out += " " + entry.write() + " " + sealed(value, file_key).write();
        const std::string placeholder = "[0 0000000000 0000000000 0000000000]";
        out += " /ByteRange ";
        const std::size_t byte_range_at = out.size();
        out += placeholder + " /Contents ";
        const std::size_t contents_at = out.size();
        out += "<" + std::string(reserved * 2, '0') + ">";
        const std::size_t contents_end = out.size();
        out += ">>\nendobj\n";

        // The cross-reference section: a table after a table, a stream after a stream.
        const auto [previous, table] = last_section(pdf);
        std::string id_first;
        if (const auto *ids = trailer.find("ID"); ids && ids->is<array>() && !ids->as<array>().empty() && ids->as<array>()[0].is<string>())
            id_first = ids->as<array>()[0].as<string>().bytes();
        std::string id_new(16, '\0');
        RAND_bytes(reinterpret_cast<unsigned char *>(id_new.data()), 16);
        if (id_first.empty()) id_first = id_new;
        dictionary update{{"Size", static_cast<std::int64_t>(next)}, {"Root", reference{root}}};
        if (const auto *info = trailer.find("Info")) update.add("Info", *info);
        if (const auto *encrypt = trailer.find("Encrypt")) update.add("Encrypt", *encrypt);
        update.add("ID", array{string{id_first, true}, string{id_new, true}});
        update.add("Prev", static_cast<std::int64_t>(previous));
        if (table) {
            const std::uint64_t at = out.size();
            out += "xref\n";
            for (auto run = offsets.begin(); run != offsets.end();) {
                auto end = run;
                std::uint32_t last = run->first;
                while (std::next(end) != offsets.end() && std::next(end)->first == last + 1) last = (++end)->first;
                ++end;
                out += std::to_string(run->first) + " " + std::to_string(last - run->first + 1) + "\n";
                for (; run != end; ++run) {
                    char entry[24];
                    std::snprintf(entry, sizeof entry, "%010llu 00000 n \n", static_cast<unsigned long long>(run->second));
                    out += entry;
                }
            }
            out += "trailer\n" + update.write() + "\nstartxref\n" + std::to_string(at) + "\n%%EOF\n";
        } else {
            // A cross-reference stream (7.5.8), itself one more object; never encrypted.
            const std::uint32_t xref_id = next++;
            update.set("Size", static_cast<std::int64_t>(next));
            const std::uint64_t at = out.size();
            offsets[xref_id] = at;
            array index;
            std::string rows;
            for (const auto &[id, offset]: offsets) {
                index.push_back(static_cast<std::int64_t>(id));
                index.push_back(1);
                rows += '\x01';
                for (int shift = 56; shift >= 0; shift -= 8) rows += static_cast<char>(offset >> shift & 0xFF);
                rows += std::string(2, '\0');
            }
            update.add("Type", name{"XRef"});
            update.add("W", array{1, 8, 2});
            update.add("Index", index);
            update.add("Length", static_cast<std::int64_t>(rows.size()));
            out += std::to_string(xref_id) + " 0 obj\n" + update.write() + "\nstream\n" + rows + "\nendstream\nendobj\nstartxref\n" + std::to_string(at) +
                   "\n%%EOF\n";
        }

        // What's signed: everything but /Contents' hex.
        const std::string range = "[0 " + padded(contents_at) + " " + padded(contents_end) + " " + padded(out.size() - contents_end) + "]";
        out.replace(byte_range_at, placeholder.size(), range);
        const std::string covered = out.substr(0, contents_at) + out.substr(contents_end);
        const std::string signature = signed_data(covered, state, options);
        if (signature.size() > reserved)
            throw std::runtime_error("sc::pdflite: the signature (" + std::to_string(signature.size()) + " bytes) doesn't fit the room kept for it (" +
                                     std::to_string(reserved) + "): give a larger reserved");
        const std::string hex = to_hex(signature);
        out.replace(contents_at + 1, hex.size(), hex);
        return out;
    }

    void sign(const std::filesystem::path &input, const std::filesystem::path &output, const signing_key &key, const signature_options &options) {
        std::ifstream in{input, std::ios::binary};
        if (!in) throw std::invalid_argument("sc::pdflite: can't read " + input.string());
        const std::string bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        const std::string signed_bytes = sign(bytes, key, options);
        const auto temporary = std::filesystem::path{output.string() + ".signing"};
        {
            std::ofstream out{temporary, std::ios::binary | std::ios::trunc};
            out.write(signed_bytes.data(), static_cast<std::streamsize>(signed_bytes.size()));
            if (!out) throw std::runtime_error("sc::pdflite: can't write " + temporary.string());
        }
        std::filesystem::rename(temporary, output);
    }

    std::vector<signature_check> verify(const std::string_view pdf, const std::string_view password) {
        std::optional<reader> opened;
        try {
            opened.emplace(reader::from_memory(std::string{pdf}, password));
        } catch (const std::exception &error) {
            throw std::invalid_argument(error.what());
        }
        const reader &in = *opened;
        std::vector<signature_check> out;
        const dictionary catalog = in.catalog();
        const auto *form_entry = catalog.find("AcroForm");
        if (!form_entry) return out;
        const object form = in.resolve(*form_entry);
        if (!form.is<dictionary>()) return out;
        const auto *fields_entry = form.as<dictionary>().find("Fields");
        if (!fields_entry) return out;

        std::vector<object> fields;
        std::function<void(const object &, int)> collect = [&](const object &list, const int depth) {
            const object items = in.resolve(list);
            if (!items.is<array>() || depth > 8) return;
            for (const auto &item: items.as<array>()) {
                const object field = in.resolve(item);
                if (!field.is<dictionary>()) continue;
                if (const auto *kids = field.as<dictionary>().find("Kids")) collect(*kids, depth + 1);
                else fields.push_back(field);
            }
        };
        collect(*fields_entry, 0);

        const auto text_of = [&](const dictionary &entries, const std::string_view key) -> std::string {
            const auto *value = entries.find(key);
            if (!value) return {};
            const object resolved = in.resolve(*value);
            if (!resolved.is<string>()) return {};
            const std::string &bytes = resolved.as<string>().bytes();
            if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFE && static_cast<unsigned char>(bytes[1]) == 0xFF) {
                // UTF-16BE text: to UTF-8.
                std::string utf8;
                for (std::size_t i = 2; i + 1 < bytes.size(); i += 2) {
                    char32_t code = static_cast<unsigned char>(bytes[i]) << 8 | static_cast<unsigned char>(bytes[i + 1]);
                    if (code >= 0xD800 && code < 0xDC00 && i + 3 < bytes.size()) {
                        const char32_t low = static_cast<unsigned char>(bytes[i + 2]) << 8 | static_cast<unsigned char>(bytes[i + 3]);
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                        i += 2;
                    }
                    if (code < 0x80) utf8 += static_cast<char>(code);
                    else if (code < 0x800) utf8 += {static_cast<char>(0xC0 | code >> 6), static_cast<char>(0x80 | (code & 0x3F))};
                    else if (code < 0x10000)
                        utf8 += {static_cast<char>(0xE0 | code >> 12), static_cast<char>(0x80 | (code >> 6 & 0x3F)), static_cast<char>(0x80 | (code & 0x3F))};
                    else
                        utf8 += {static_cast<char>(0xF0 | code >> 18), static_cast<char>(0x80 | (code >> 12 & 0x3F)), static_cast<char>(0x80 | (code >> 6 & 0x3F)),
                                 static_cast<char>(0x80 | (code & 0x3F))};
                }
                return utf8;
            }
            return bytes;
        };

        for (const auto &field_value: fields) {
            const auto &field = field_value.as<dictionary>();
            const auto *type = field.find("FT");
            if (!type || !type->is<name>() || type->as<name>().text() != "Sig") continue;
            const auto *value = field.find("V");
            if (!value) continue;
            const object signature_value = in.resolve(*value);
            if (!signature_value.is<dictionary>()) continue;
            const auto &signature = signature_value.as<dictionary>();
            signature_check check;
            check.field = text_of(field, "T");
            check.signed_at = text_of(signature, "M");
            check.reason = text_of(signature, "Reason");
            check.location = text_of(signature, "Location");
            check.name = text_of(signature, "Name");
            if (const auto *filter = signature.find("SubFilter"); filter && filter->is<name>()) check.sub_filter = filter->as<name>().text();

            const auto *range_entry = signature.find("ByteRange");
            const object range = range_entry ? in.resolve(*range_entry) : object{};
            if (!range.is<array>() || range.as<array>().size() != 4 ||
                !std::ranges::all_of(range.as<array>(), [](const object &n) { return n.is<std::int64_t>() && n.as<std::int64_t>() >= 0; })) {
                check.error = "no usable /ByteRange";
                out.push_back(std::move(check));
                continue;
            }
            const auto at = [&](const std::size_t i) { return static_cast<std::uint64_t>(range.as<array>()[i].as<std::int64_t>()); };
            if (at(0) != 0 || at(1) > at(2) || at(2) + at(3) > pdf.size()) {
                check.error = "its /ByteRange is outside the file";
                out.push_back(std::move(check));
                continue;
            }
            check.covers_whole_file = at(2) + at(3) == pdf.size();
            // The signature's hex, read from the file itself (the reader would decrypt it).
            const std::string_view gap = pdf.substr(at(1), at(2) - at(1));
            if (gap.size() < 2 || gap.front() != '<' || gap.back() != '>') {
                check.error = "/Contents isn't where /ByteRange leaves a gap";
                out.push_back(std::move(check));
                continue;
            }
            const std::string der = from_hex(gap.substr(1, gap.size() - 2));
            const std::string covered = std::string{pdf.substr(0, at(1))} + std::string{pdf.substr(at(2), at(3))};
            const auto *der_at = reinterpret_cast<const unsigned char *>(der.data());
            cms_info cms{d2i_CMS_ContentInfo(nullptr, &der_at, static_cast<long>(der.size()))};
            if (!cms) {
                ERR_clear_error();
                check.error = "/Contents isn't a CMS signature";
                out.push_back(std::move(check));
                continue;
            }
            bio content = memory(covered);
            check.intact = CMS_verify(cms.get(), nullptr, nullptr, content.get(), nullptr, CMS_BINARY | CMS_NO_SIGNER_CERT_VERIFY) == 1;
            if (!check.intact) check.error = "the signature doesn't match the file: " + openssl_error();
            if (STACK_OF(X509) *signers = CMS_get0_signers(cms.get())) {
                if (sk_X509_num(signers) > 0) check.subject = name_of(sk_X509_value(signers, 0));
                sk_X509_free(signers);
            }
            ERR_clear_error();
            if (STACK_OF(CMS_SignerInfo) *signer_infos = CMS_get0_SignerInfos(cms.get()); signer_infos && sk_CMS_SignerInfo_num(signer_infos) > 0) {
                CMS_SignerInfo *signer = sk_CMS_SignerInfo_value(signer_infos, 0);
                check.pades = check.sub_filter == "ETSI.CAdES.detached" && CMS_signed_get_attr_by_NID(signer, NID_id_smime_aa_signingCertificateV2, -1) >= 0 &&
                              CMS_signed_get_attr_by_NID(signer, NID_pkcs9_signingTime, -1) < 0;
                if (const int token_at = CMS_unsigned_get_attr_by_NID(signer, NID_id_smime_aa_timeStampToken, -1); token_at >= 0) {
                    check.timestamped = true;
                    const ASN1_TYPE *token_type = X509_ATTRIBUTE_get0_type(CMS_unsigned_get_attr(signer, token_at), 0);
                    if (token_type && token_type->type == V_ASN1_SEQUENCE) {
                        const ASN1_STRING *sequence = token_type->value.sequence;
                        const unsigned char *token_der = ASN1_STRING_get0_data(sequence);
                        owned<PKCS7, PKCS7_free> token{d2i_PKCS7(nullptr, &token_der, static_cast<long>(length_of(sequence)))};
                        owned<TS_TST_INFO, TS_TST_INFO_free> info{token ? PKCS7_to_TS_TST_INFO(token.get()) : nullptr};
                        if (info) {
                            const ASN1_GENERALIZEDTIME *time = TS_TST_INFO_get_time(info.get());
                            check.timestamp_time.assign(reinterpret_cast<const char *>(ASN1_STRING_get0_data(time)), static_cast<std::size_t>(length_of(time)));
                            const ASN1_OCTET_STRING *value_string = CMS_SignerInfo_get0_signature(signer);
                            const std::string imprint = sha256({reinterpret_cast<const char *>(ASN1_STRING_get0_data(value_string)),
                                                                static_cast<std::size_t>(length_of(value_string))});
                            const ASN1_OCTET_STRING *digest = TS_MSG_IMPRINT_get_msg(TS_TST_INFO_get_msg_imprint(info.get()));
                            const bool matches = std::string_view{reinterpret_cast<const char *>(ASN1_STRING_get0_data(digest)),
                                                                  static_cast<std::size_t>(length_of(digest))} == imprint;
                            // The token's own signature (its signer not checked against a trust list).
                            bio nothing{BIO_new(BIO_s_mem())};
                            const bool holds = PKCS7_verify(token.get(), nullptr, nullptr, nullptr, nothing.get(), PKCS7_NOVERIFY) == 1;
                            check.timestamp_intact = matches && holds;
                        }
                    }
                    ERR_clear_error();
                }
            }
            out.push_back(std::move(check));
        }
        return out;
    }
}
