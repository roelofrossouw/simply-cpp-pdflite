#ifndef SC_PDFLITE_SIGN_H
#define SC_PDFLITE_SIGN_H

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Digital signatures (ISO 32000-2, 12.8): PAdES baseline signatures (ETSI EN 319 142-1), B-B, and
// B-T with a time-stamp authority. A signature is added to a finished PDF as an incremental update,
// so what was there stays byte for byte: an encrypted file stays encrypted (its owner password and
// permissions too), a signature already on it stays valid. OpenSSL's libcrypto does the
// cryptography (CMS, SHA-256, RFC 3161 requests).
namespace sc::pdflite {
    // What a signature says besides who signed, and how it's made.
    struct signature_options {
        // The signer's name (/Name; default: the certificate's common name), why and where they
        // signed, and how to reach them: shown by readers in the signature's details.
        std::string name;
        std::string reason;
        std::string location;
        std::string contact;
        // The signature field's name (/T); "Signature1", or the next free number when it's taken.
        std::string field;
        // A time-stamp authority (RFC 3161) for PAdES B-T: given the DER of a time-stamp request,
        // returns the DER of the authority's response (an HTTP POST of application/timestamp-query
        // to its URL, say). The token proves when the signature existed. Empty: B-B.
        std::function<std::string(std::string_view request)> timestamp;
        // Room kept for the signature (bytes; written in hex, twice as long). 0 works it out from
        // the certificates, with room for a time-stamp token when there's an authority.
        std::size_t reserved = 0;
        // Opens an encrypted file: its user or owner password (empty for a file with an owner
        // password only, which anyone may open).
        std::string password;
    };

    // A signer's certificate, its private key, and the certificates between it and the root (put in
    // the signature, so a reader can build the chain). The private key never leaves the process.
    class signing_key {
    public:
        // From PEM: certificate holds the signer's certificate, then any others of its chain;
        // chain may hold more; key is the private key, encrypted with passphrase or not. Throws
        // std::invalid_argument when they can't be read or the key isn't the certificate's.
        [[nodiscard]] static signing_key from_pem(std::string_view certificate, std::string_view key, std::string_view passphrase = {},
                                                  std::string_view chain = {});

        // From a PKCS#12 file's bytes (.p12, .pfx): certificate, key and chain. Throws
        // std::invalid_argument when it can't be read (a wrong passphrase among them).
        [[nodiscard]] static signing_key from_pkcs12(std::string_view bytes, std::string_view passphrase);

        // The signer's distinguished name, RFC 2253: "CN=Roelof,O=Example".
        [[nodiscard]] std::string subject() const;

        struct impl;

    private:
        explicit signing_key(std::shared_ptr<impl> state) : impl_(std::move(state)) {}

        friend std::string sign(std::string_view, const signing_key &, const signature_options &);

        std::shared_ptr<impl> impl_;
    };

    // pdf with a PAdES signature added (an invisible signature field on the last page) as an
    // incremental update: the signature covers the whole file. The signer's certificate is in the
    // signature (signing-certificate-v2), the time it claims is the signature dictionary's /M, and
    // nothing in the signature names a signing time, as PAdES asks. Throws std::invalid_argument
    // when pdf can't be read or opened, std::runtime_error when OpenSSL or the time-stamp
    // authority fails or the signature doesn't fit in reserved.
    [[nodiscard]] std::string sign(std::string_view pdf, const signing_key &key, const signature_options &options = {});

    // The same from file to file (output may be input).
    void sign(const std::filesystem::path &input, const std::filesystem::path &output, const signing_key &key,
              const signature_options &options = {});

    // A signature found in a file, checked: whether the bytes it covers are unchanged and its
    // cryptography holds. Who signed isn't checked against any trust list: subject says who it
    // claims, for the caller to trust or not.
    struct signature_check {
        std::string field;      // the signature field's name
        std::string subject;    // the signer's certificate's subject, RFC 2253
        std::string sub_filter; // ETSI.CAdES.detached (PAdES), adbe.pkcs7.detached ...
        std::string signed_at;  // /M as written: D:20261124120000+02'00'
        std::string reason, location, name;
        bool covers_whole_file = false; // false: the file was changed after it (an incremental update)
        bool intact = false;            // the covered bytes' digest matches, and the signature over it holds
        bool pades = false;             // ETSI.CAdES.detached, signing-certificate-v2, no signing time
        bool timestamped = false;       // a time-stamp token on the signature (B-T)
        std::string timestamp_time;     // its time, as the authority gave it (GeneralizedTime)
        bool timestamp_intact = false;  // the token's digest matches the signature, and the token's signature holds
        std::string error;              // why it isn't intact, when it isn't

        // Intact, covering the whole file, and (when there's one) with an intact time-stamp.
        [[nodiscard]] bool valid() const { return intact && covers_whole_file && (!timestamped || timestamp_intact); }
    };

    // Every signature in pdf, in the order of its fields. Throws std::invalid_argument when pdf
    // can't be read or opened (password as sign() takes it).
    [[nodiscard]] std::vector<signature_check> verify(std::string_view pdf, std::string_view password = {});
}

#endif //SC_PDFLITE_SIGN_H
