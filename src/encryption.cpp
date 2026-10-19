#include "pdflite/encryption.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <memory>
#include <stdexcept>

namespace sc::pdflite {
    namespace {
        [[noreturn]] void failed(const std::string &what) { throw std::runtime_error("sc::pdflite: OpenSSL failed to " + what); }

        std::string random_bytes(const std::size_t count) {
            std::string out(count, '\0');
            if (RAND_bytes(reinterpret_cast<unsigned char *>(out.data()), static_cast<int>(count)) != 1) failed("make random bytes");
            return out;
        }

        // AES in CBC (iv given) or ECB (no iv) mode, padded (PKCS#7) or not; encrypting or decrypting.
        std::string aes(const std::string_view key, const std::string_view iv, const std::string_view data, const bool encrypting, const bool padded) {
            const EVP_CIPHER *cipher = key.size() == 16 ? (iv.empty() ? EVP_aes_128_ecb() : EVP_aes_128_cbc())
                                                        : (iv.empty() ? EVP_aes_256_ecb() : EVP_aes_256_cbc());
            const std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> context{EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free};
            if (!context || EVP_CipherInit_ex(context.get(), cipher, nullptr, reinterpret_cast<const unsigned char *>(key.data()),
                                              iv.empty() ? nullptr : reinterpret_cast<const unsigned char *>(iv.data()), encrypting ? 1 : 0) != 1)
                failed("start AES");
            EVP_CIPHER_CTX_set_padding(context.get(), padded ? 1 : 0);
            std::string out(data.size() + 16, '\0');
            int written = 0, last = 0;
            if (EVP_CipherUpdate(context.get(), reinterpret_cast<unsigned char *>(out.data()), &written, reinterpret_cast<const unsigned char *>(data.data()),
                                 static_cast<int>(data.size())) != 1)
                failed("run AES");
            if (EVP_CipherFinal_ex(context.get(), reinterpret_cast<unsigned char *>(out.data()) + written, &last) != 1) {
                if (!encrypting) throw std::invalid_argument("sc::pdflite: data that doesn't decrypt with this key");
                failed("finish AES");
            }
            out.resize(static_cast<std::size_t>(written + last));
            return out;
        }

        std::string digest(const EVP_MD *kind, const std::string_view data) {
            unsigned char out[EVP_MAX_MD_SIZE];
            unsigned int size = 0;
            if (EVP_Digest(data.data(), data.size(), out, &size, kind, nullptr) != 1) failed("hash");
            return {reinterpret_cast<char *>(out), size};
        }

        // A password as revision 6 takes it: UTF-8, at most 127 bytes.
        std::string_view prepared(const std::string_view password) { return password.substr(0, 127); }

        // Algorithm 2.B (7.6.4.3.4): the hash of a password with a salt (and, for the owner's, the
        // 48-byte U string), 64 rounds or more of SHA-256, -384 or -512 chosen by AES-128's output.
        std::string hash(const std::string_view password, const std::string_view salt, const std::string_view user = {}) {
            std::string k = digest(EVP_sha256(), std::string{password} + std::string{salt} + std::string{user});
            for (int round = 0;; ++round) {
                std::string block = std::string{password} + k + std::string{user};
                std::string repeated;
                repeated.reserve(block.size() * 64);
                for (int i = 0; i < 64; ++i) repeated += block;
                const std::string e = aes(k.substr(0, 16), k.substr(16, 16), repeated, true, false);
                int sum = 0;
                for (int i = 0; i < 16; ++i) sum += static_cast<unsigned char>(e[i]);
                k = digest(sum % 3 == 0 ? EVP_sha256() : sum % 3 == 1 ? EVP_sha384() : EVP_sha512(), e);
                if (round >= 63 && static_cast<unsigned char>(e.back()) <= round - 31) break;
            }
            return k.substr(0, 32);
        }
    }

    std::int32_t permissions::bits() const {
        // Bits 7, 8 and 13 to 32 set, 1 and 2 clear (table 22); 10 (accessibility) always set.
        std::uint32_t p = 0xFFFFF0C0u | 1u << 9;
        if (print) p |= 1u << 2;
        if (modify) p |= 1u << 3;
        if (copy) p |= 1u << 4;
        if (annotate) p |= 1u << 5;
        if (fill_forms) p |= 1u << 8;
        if (assemble) p |= 1u << 10;
        if (print && print_high_quality) p |= 1u << 11;
        return static_cast<std::int32_t>(p);
    }

    encryption::encryption(const protection &settings) {
        if (settings.user_password.empty() && settings.owner_password.empty())
            throw std::invalid_argument("sc::pdflite: protect() without a password: nothing would be protected");
        const auto user_password = prepared(settings.user_password);
        const auto owner_password = prepared(settings.owner_password.empty() ? settings.user_password : settings.owner_password);
        key_ = random_bytes(32);

        // Algorithm 8: U (the user password's hash with a validation salt, then that salt and a key
        // salt) and UE (the file key, wrapped with a hash keyed by the key salt).
        const std::string user_salts = random_bytes(16);
        const std::string u = hash(user_password, user_salts.substr(0, 8)) + user_salts;
        const std::string ue = aes(hash(user_password, user_salts.substr(8, 8)), std::string(16, '\0'), key_, true, false);
        // Algorithm 9: O and OE alike for the owner password, hashed with U.
        const std::string owner_salts = random_bytes(16);
        const std::string o = hash(owner_password, owner_salts.substr(0, 8), u) + owner_salts;
        const std::string oe = aes(hash(owner_password, owner_salts.substr(8, 8), u), std::string(16, '\0'), key_, true, false);
        // Algorithm 10: Perms, the permissions sealed with the file key, so they can't be changed.
        const auto p = static_cast<std::uint32_t>(settings.permissions.bits());
        std::string perms;
        for (int i = 0; i < 4; ++i) perms += static_cast<char>(p >> (8 * i) & 0xFF);
        perms += "\xFF\xFF\xFF\xFF";
        perms += "Tadb"; // T: the metadata is encrypted too
        perms += random_bytes(4);
        perms = aes(key_, {}, perms, true, false);

        dictionary_ = pdflite::dictionary{
            {"Filter", name{"Standard"}}, {"V", 5}, {"R", 6}, {"Length", 256},
            {"CF", pdflite::dictionary{{"StdCF", pdflite::dictionary{{"AuthEvent", name{"DocOpen"}}, {"CFM", name{"AESV3"}}, {"Length", 32}}}}},
            {"StmF", name{"StdCF"}}, {"StrF", name{"StdCF"}},
            {"O", string{o, true}}, {"U", string{u, true}}, {"OE", string{oe, true}}, {"UE", string{ue, true}},
            {"P", static_cast<std::int64_t>(settings.permissions.bits())}, {"Perms", string{perms, true}}, {"EncryptMetadata", true}};
    }

    std::string encryption::encrypt(const std::string_view data) const {
        const std::string iv = random_bytes(16);
        return iv + aes(key_, iv, data, true, true);
    }

    std::optional<std::string> encryption::key_for(const pdflite::dictionary &encrypt, const std::string_view password) {
        const auto *revision = encrypt.find("R");
        if (!revision || !revision->is<std::int64_t>() || revision->as<std::int64_t>() != 6)
            throw std::invalid_argument("sc::pdflite: an encryption dictionary that isn't revision 6");
        const auto bytes = [&](const std::string_view key) { return encrypt.at(key).as<string>().bytes(); };
        const std::string u = bytes("U"), o = bytes("O");
        if (u.size() < 48 || o.size() < 48) throw std::invalid_argument("sc::pdflite: an encryption dictionary with short O or U");
        const auto pw = prepared(password);
        // The owner's first (7.6.4.4.10), then the user's (7.6.4.4.11).
        if (hash(pw, o.substr(32, 8), u.substr(0, 48)) == o.substr(0, 32))
            return aes(hash(pw, o.substr(40, 8), u.substr(0, 48)), std::string(16, '\0'), bytes("OE"), false, false);
        if (hash(pw, u.substr(32, 8)) == u.substr(0, 32))
            return aes(hash(pw, u.substr(40, 8)), std::string(16, '\0'), bytes("UE"), false, false);
        return std::nullopt;
    }

    std::string encryption::decrypt(const std::string_view key, const std::string_view data) {
        if (data.size() < 32 || data.size() % 16 != 0) throw std::invalid_argument("sc::pdflite: data that isn't AES-256 encrypted");
        return aes(key, data.substr(0, 16), data.substr(16), false, true);
    }
}
