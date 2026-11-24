// sc::pdflite::sign() and verify(): PAdES B-B and B-T signatures on plain and owner-password files,
// with throwaway certificates made here (a signer, and a time-stamp authority answering in
// process), PEM and PKCS#12 keys, a second signature, and files changed after signing. The signed
// files go to SC_PDFLITE_OUTPUT too, for ctest's check-* tests (and pdfsig, when it's installed).
#include <sc/pdflite/pdflite.h>
#include <sc/pdflite/sign.h>

#include "sc_test.h"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/ts.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using namespace sc::pdflite;

namespace {
    fs::path output() {
        const char *where = std::getenv("SC_PDFLITE_OUTPUT");
        fs::path out = where && *where ? fs::path{where} : fs::temp_directory_path() / "sc-pdflite";
        fs::create_directories(out);
        return out;
    }

    std::string read(const fs::path &path) {
        std::ifstream in{path, std::ios::binary};
        return {std::istreambuf_iterator<char>{in}, {}};
    }

    void write(const fs::path &path, const std::string &bytes) {
        std::ofstream out{path, std::ios::binary | std::ios::trunc};
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }

    // A throwaway key and self-signed certificate; a time-stamp authority's when tsa.
    struct credentials {
        EVP_PKEY *key = nullptr;
        X509 *certificate = nullptr;

        credentials(const char *common_name, const bool tsa) {
            key = EVP_RSA_gen(2048);
            certificate = X509_new();
            X509_set_version(certificate, 2);
            ASN1_INTEGER_set(X509_get_serialNumber(certificate), tsa ? 2 : 1);
            X509_gmtime_adj(X509_getm_notBefore(certificate), -3600);
            X509_gmtime_adj(X509_getm_notAfter(certificate), 3600L * 24 * 30);
            X509_set_pubkey(certificate, key);
            X509_NAME *subject = X509_NAME_new();
            X509_NAME_add_entry_by_txt(subject, "O", MBSTRING_ASC, reinterpret_cast<const unsigned char *>("Example Test"), -1, -1, 0);
            X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>(common_name), -1, -1, 0);
            X509_set_subject_name(certificate, subject);
            X509_set_issuer_name(certificate, subject);
            X509_NAME_free(subject);
            X509V3_CTX context;
            X509V3_set_ctx_nodb(&context);
            X509V3_set_ctx(&context, certificate, certificate, nullptr, nullptr, 0);
            const auto extend = [&](const int nid, const char *value) {
                if (X509_EXTENSION *extension = X509V3_EXT_conf_nid(nullptr, &context, nid, value)) {
                    X509_add_ext(certificate, extension, -1);
                    X509_EXTENSION_free(extension);
                }
            };
            extend(NID_key_usage, "critical,digitalSignature,nonRepudiation");
            if (tsa) extend(NID_ext_key_usage, "critical,timeStamping");
            X509_sign(certificate, key, EVP_sha256());
        }

        ~credentials() {
            X509_free(certificate);
            EVP_PKEY_free(key);
        }

        [[nodiscard]] std::string certificate_pem() const {
            BIO *out = BIO_new(BIO_s_mem());
            PEM_write_bio_X509(out, certificate);
            char *text = nullptr;
            const long size = BIO_get_mem_data(out, &text);
            std::string pem{text, static_cast<std::size_t>(size)};
            BIO_free(out);
            return pem;
        }

        [[nodiscard]] std::string key_pem(const char *passphrase = nullptr) const {
            BIO *out = BIO_new(BIO_s_mem());
            PEM_write_bio_PrivateKey(out, key, passphrase ? EVP_aes_256_cbc() : nullptr, nullptr, 0, nullptr, const_cast<char *>(passphrase));
            char *text = nullptr;
            const long size = BIO_get_mem_data(out, &text);
            std::string pem{text, static_cast<std::size_t>(size)};
            BIO_free(out);
            return pem;
        }

        [[nodiscard]] std::string pkcs12(const char *passphrase) const {
            PKCS12 *p12 = PKCS12_create(passphrase, "test", key, certificate, nullptr, 0, 0, 0, 0, 0);
            const int size = i2d_PKCS12(p12, nullptr);
            std::string der(static_cast<std::size_t>(size), '\0');
            auto *at = reinterpret_cast<unsigned char *>(der.data());
            i2d_PKCS12(p12, &at);
            PKCS12_free(p12);
            return der;
        }
    };

    // A time-stamp authority in process: OpenSSL's TS_RESP_CTX answers the request.
    std::string time_stamp(const credentials &tsa, const std::string_view request) {
        TS_RESP_CTX *context = TS_RESP_CTX_new();
        TS_RESP_CTX_set_signer_cert(context, tsa.certificate);
        TS_RESP_CTX_set_signer_key(context, tsa.key);
        ASN1_OBJECT *policy = OBJ_txt2obj("1.3.6.1.4.1.99999.1", 1);
        TS_RESP_CTX_set_def_policy(context, policy);
        TS_RESP_CTX_add_md(context, EVP_sha256());
        TS_RESP_CTX_set_signer_digest(context, EVP_sha256());
        BIO *in = BIO_new_mem_buf(request.data(), static_cast<int>(request.size()));
        TS_RESP *response = TS_RESP_create_response(context, in);
        std::string der;
        if (response) {
            const int size = i2d_TS_RESP(response, nullptr);
            der.resize(static_cast<std::size_t>(size));
            auto *at = reinterpret_cast<unsigned char *>(der.data());
            i2d_TS_RESP(response, &at);
            TS_RESP_free(response);
        }
        BIO_free(in);
        ASN1_OBJECT_free(policy);
        TS_RESP_CTX_free(context);
        return der;
    }

    // A two-page PDF, with an owner password only when protected.
    std::string sample(const bool protect_it) {
        const fs::path path = fs::temp_directory_path() / (protect_it ? "sc-pdflite-sign-protected.pdf" : "sc-pdflite-sign-plain.pdf");
        {
            document pdf{path};
            const auto text = [&](const char *words) {
                const auto font = pdf.standard_font(sc::graphics::standard_font::face::helvetica);
                auto &page = pdf.page_content();
                page.begin_text();
                page.font(font, 12);
                page.text_position(72, 750);
                page.show(words);
                page.end_text();
            };
            text("Page one");
            pdf.add_page();
            text("Page two");
            pdf.info("Title", "Signed sample");
            if (protect_it) pdf.protect({.user_password = "", .owner_password = "owner-secret", .permissions = {.copy = true}});
            pdf.close();
        }
        std::string bytes = read(path);
        fs::remove(path);
        return bytes;
    }
}

int main() {
    const credentials signer{"Test Signer", false};
    const credentials authority{"Test TSA", true};
    const auto key = signing_key::from_pem(signer.certificate_pem(), signer.key_pem());
    const fs::path out = output();

    SECTION("Keys: PEM (with a passphrase), PKCS#12, and ones that don't fit");
    {
        CHECK_EQ(key.subject(), std::string{"CN=Test Signer,O=Example Test"});
        CHECK_EQ(signing_key::from_pem(signer.certificate_pem(), signer.key_pem("pass"), "pass").subject(), key.subject());
        CHECK_THROWS_AS(signing_key::from_pem(signer.certificate_pem(), signer.key_pem("pass"), "wrong"), std::exception);
        CHECK_THROWS_AS(signing_key::from_pem(signer.certificate_pem(), authority.key_pem()), std::exception); // another's key
        CHECK_THROWS_AS(signing_key::from_pem("not a certificate", signer.key_pem()), std::exception);
        CHECK_EQ(signing_key::from_pkcs12(signer.pkcs12("p12pass"), "p12pass").subject(), key.subject());
        CHECK_THROWS_AS(signing_key::from_pkcs12(signer.pkcs12("p12pass"), "nope"), std::exception);
    }

    SECTION("PAdES B-B on a plain file");
    {
        const std::string original = sample(false);
        const std::string signed_bytes = sign(original, key, {.reason = "Approved", .location = "Head office"});
        write(out / "signed-b-b.pdf", signed_bytes);
        CHECK(signed_bytes.starts_with(original)); // an incremental update: the file as it was, then more
        const auto checks = verify(signed_bytes);
        CHECK_EQ(checks.size(), std::size_t{1});
        if (!checks.empty()) {
            const auto &check = checks[0];
            CHECK(check.valid());
            CHECK(check.intact);
            CHECK(check.covers_whole_file);
            CHECK(check.pades);
            CHECK(!check.timestamped);
            CHECK_EQ(check.field, std::string{"Signature1"});
            CHECK_EQ(check.subject, key.subject());
            CHECK_EQ(check.sub_filter, std::string{"ETSI.CAdES.detached"});
            CHECK_EQ(check.reason, std::string{"Approved"});
            CHECK_EQ(check.location, std::string{"Head office"});
            CHECK_EQ(check.name, std::string{"Test Signer"});
            CHECK(check.signed_at.starts_with("D:20"));
            CHECK_EQ(check.error, std::string{});
        }
        const reader again = reader::from_memory(signed_bytes);
        CHECK_EQ(again.page_count(), std::size_t{2});
        CHECK(again.catalog().contains("AcroForm"));
        CHECK(verify(original).empty());

        // A byte changed in what's covered: no longer intact.
        std::string tampered = signed_bytes;
        tampered[tampered.find("Signed sample")] = 'Z'; // the title, in the covered part
        const auto broken = verify(tampered);
        CHECK(!broken.empty() && !broken[0].intact && !broken[0].valid());
        CHECK(!broken.empty() && !broken[0].error.empty());

        // Something appended after signing: still intact, but not the whole file.
        const auto appended = verify(signed_bytes + "% a comment added later\n");
        CHECK(!appended.empty() && appended[0].intact && !appended[0].covers_whole_file && !appended[0].valid());

        // Signed again: two signatures, the first covering the file as it was then.
        const std::string twice = sign(signed_bytes, signing_key::from_pkcs12(signer.pkcs12("p"), "p"), {.field = "Approval"});
        write(out / "signed-twice.pdf", twice);
        const auto both = verify(twice);
        CHECK_EQ(both.size(), std::size_t{2});
        if (both.size() == 2) {
            CHECK(both[0].intact && !both[0].covers_whole_file);
            CHECK(both[1].valid());
            CHECK_EQ(both[1].field, std::string{"Approval"});
        }
    }

    SECTION("An owner-password file: signed, still encrypted, its permissions kept");
    {
        const std::string original = sample(true);
        const std::string signed_bytes = sign(original, key);
        write(out / "signed-protected.pdf", signed_bytes);
        const auto checks = verify(signed_bytes);
        CHECK_EQ(checks.size(), std::size_t{1});
        CHECK(!checks.empty() && checks[0].valid() && checks[0].pades);
        CHECK(!checks.empty() && checks[0].name == "Test Signer"); // an encrypted string, read back
        const reader again = reader::from_memory(signed_bytes);
        CHECK(again.encrypted());
        CHECK_EQ(again.page_count(), std::size_t{2});
        const reader before = reader::from_memory(original);
        const std::uint32_t handler = before.trailer().at("Encrypt").as<reference>().id;
        CHECK_EQ(again.trailer().at("Encrypt").as<reference>().id, handler); // the same security handler, so the same permissions
        CHECK_THROWS_AS(sign(original, key, {.password = "wrong"}), std::exception);
    }

    SECTION("PAdES B-T: a time-stamp authority's token on the signature");
    {
        signature_options options;
        options.timestamp = [&](const std::string_view request) { return time_stamp(authority, request); };
        const std::string signed_bytes = sign(sample(false), key, options);
        write(out / "signed-b-t.pdf", signed_bytes);
        const auto checks = verify(signed_bytes);
        CHECK_EQ(checks.size(), std::size_t{1});
        if (!checks.empty()) {
            CHECK(checks[0].valid());
            CHECK(checks[0].timestamped);
            CHECK(checks[0].timestamp_intact);
            CHECK(checks[0].timestamp_time.size() >= 14);
        }
        signature_options refusing;
        refusing.timestamp = [](std::string_view) { return std::string{"not a response"}; };
        CHECK_THROWS_AS(sign(sample(false), key, refusing), std::exception);
    }

    SECTION("Too little room, and what isn't a PDF");
    {
        CHECK_THROWS_AS(sign(sample(false), key, {.reserved = 100}), std::exception);
        CHECK_THROWS_AS(sign("not a pdf", key), std::exception);
        CHECK_THROWS_AS(verify("not a pdf"), std::exception);
    }

    TEST_SUMMARY();
}
