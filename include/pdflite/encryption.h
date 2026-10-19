#ifndef SC_PDFLITE_ENCRYPTION_H
#define SC_PDFLITE_ENCRYPTION_H

#include "object.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// sc-pdflite's encryption (ISO 32000-2, 7.6: the standard security handler, revision 6): AES-256,
// with OpenSSL's libcrypto doing the cryptography.
namespace sc::pdflite {
    // What a reader without the owner's password may do (table 22). Readers honour these; only
    // the user password encrypts. Extracting text for accessibility is always allowed, as PDF 2.0
    // asks.
    struct permissions {
        bool print = true;              // bit 3
        bool print_high_quality = true; // bit 12: else print at low resolution only
        bool copy = false;              // bit 5: copy or extract text and graphics
        bool modify = false;            // bit 4: change the content
        bool annotate = false;          // bit 6: add or change comments, fill forms
        bool fill_forms = false;        // bit 9: fill forms even when annotate is false
        bool assemble = false;          // bit 11: insert, rotate or delete pages, bookmarks

        // As /P has them: a 32-bit value, the reserved bits as the standard sets them.
        [[nodiscard]] std::int32_t bits() const;
    };

    // Passwords and permissions for document::protect(). The user password opens the file (empty:
    // anyone opens it, the permissions applying); the owner password gives every right (empty: the
    // user password). Passwords are UTF-8, at most 127 bytes counted (longer ones are cut, as the
    // standard does); they aren't SASLprep-normalised, so a password of ASCII letters and digits is
    // the safest to type in any reader.
    struct protection {
        std::string user_password;
        std::string owner_password;
        pdflite::permissions permissions;
    };

    // The security handler: a random 256-bit file key, wrapped for each password, and the /Encrypt
    // dictionary that says so (V 5, R 6, AESV3). Every string and stream is then encrypted with
    // AES-256 in CBC mode, a fresh random IV before each.
    class encryption {
    public:
        // Throws std::invalid_argument when neither password is given (nothing would be protected),
        // std::runtime_error when OpenSSL fails.
        explicit encryption(const protection &settings);

        // The /Encrypt dictionary (written unencrypted, as it has to be read first).
        [[nodiscard]] const pdflite::dictionary &dictionary() const { return dictionary_; }

        // data encrypted: a random IV, then AES-256-CBC with PKCS#7 padding (7.6.3).
        [[nodiscard]] std::string encrypt(std::string_view data) const;

        // Opening a file: the file key a password unlocks in its /Encrypt dictionary's U and UE (the
        // user password) or O and OE (the owner's), or nullopt for a wrong password. Throws
        // std::invalid_argument for a dictionary that isn't revision 6.
        [[nodiscard]] static std::optional<std::string> key_for(const pdflite::dictionary &encrypt, std::string_view password);

        // Data encrypt() made, decrypted with the file key; throws std::invalid_argument when it isn't.
        [[nodiscard]] static std::string decrypt(std::string_view key, std::string_view data);

    private:
        std::string key_;
        pdflite::dictionary dictionary_;
    };
}

#endif //SC_PDFLITE_ENCRYPTION_H
