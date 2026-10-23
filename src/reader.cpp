#include "pdflite/reader.h"

#include "pdflite/encryption.h"

#include <openssl/evp.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace sc::pdflite {
    namespace {
        [[noreturn]] void damaged(const std::string &what) { throw std::runtime_error("sc::pdflite: " + what); }

        // ---- Lexing (7.2) ----

        bool is_white(const char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\0'; }

        bool is_delimiter(const char c) {
            return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' || c == '%';
        }

        bool is_regular(const char c) { return !is_white(c) && !is_delimiter(c); }

        int hex_value(const char c) {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        // An integer as an object: within 32 bits as an integer, else (beyond what readers must
        // handle, but found in files) as a real.
        object integer(const long long value) {
            if (value >= std::numeric_limits<std::int32_t>::min() && value <= std::numeric_limits<std::int32_t>::max()) return value;
            return static_cast<double>(value);
        }

        // Parses PDF's objects out of bytes, from a position.
        class parser {
        public:
            parser(const std::string_view bytes, const std::size_t at) : bytes_(bytes), at_(at) {}

            [[nodiscard]] std::size_t position() const { return at_; }

            void seek(const std::size_t at) { at_ = at; }

            [[nodiscard]] bool done() {
                skip();
                return at_ >= bytes_.size();
            }

            // Skips white space and comments.
            void skip() {
                while (at_ < bytes_.size()) {
                    if (is_white(bytes_[at_])) {
                        ++at_;
                    } else if (bytes_[at_] == '%') {
                        while (at_ < bytes_.size() && bytes_[at_] != '\n' && bytes_[at_] != '\r') ++at_;
                    } else {
                        break;
                    }
                }
            }

            // The next run of regular characters ("obj", "123", "true"), without taking it.
            [[nodiscard]] std::string_view peek_word() {
                skip();
                std::size_t end = at_;
                while (end < bytes_.size() && is_regular(bytes_[end])) ++end;
                return bytes_.substr(at_, end - at_);
            }

            std::string_view word() {
                const auto out = peek_word();
                at_ += out.size();
                return out;
            }

            // Takes keyword when it's next; whether it was.
            bool take(const std::string_view keyword) {
                if (peek_word() != keyword) return false;
                at_ += keyword.size();
                return true;
            }

            void expect(const std::string_view keyword) {
                if (!take(keyword)) damaged("expected \"" + std::string{keyword} + "\" at byte " + std::to_string(at_));
            }

            // A non-negative integer word, or nullopt (leaving the position) when the next isn't one.
            std::optional<std::uint64_t> unsigned_integer() {
                const auto text = peek_word();
                if (text.empty() || text.size() > 19 || !std::ranges::all_of(text, [](const char c) { return c >= '0' && c <= '9'; }))
                    return std::nullopt;
                at_ += text.size();
                return std::stoull(std::string{text});
            }

            // Any object (7.3), a reference "12 0 R" included.
            object value(const int depth = 0) {
                if (depth > 100) damaged("objects nested too deeply");
                skip();
                if (at_ >= bytes_.size()) damaged("the file ends inside an object");
                const char c = bytes_[at_];
                if (c == '/') return name_value();
                if (c == '(') return literal_string();
                if (c == '<') {
                    if (at_ + 1 < bytes_.size() && bytes_[at_ + 1] == '<') return dictionary_value(depth);
                    return hex_string();
                }
                if (c == '[') {
                    ++at_;
                    array out;
                    while (true) {
                        skip();
                        if (at_ >= bytes_.size()) damaged("an array that isn't closed");
                        if (bytes_[at_] == ']') {
                            ++at_;
                            return out;
                        }
                        out.push_back(value(depth + 1));
                    }
                }
                const auto text = word();
                if (text.empty()) damaged("an unexpected '" + std::string(1, c) + "' at byte " + std::to_string(at_));
                if (text == "true") return true;
                if (text == "false") return false;
                if (text == "null") return nullptr;
                return number_or_reference(text);
            }

        private:
            object number_or_reference(const std::string_view text) {
                const bool whole = std::ranges::all_of(text, [](const char c) { return c >= '0' && c <= '9'; });
                if (whole) {
                    // "12 0 R": two integers and R.
                    const std::size_t back = at_;
                    if (const auto generation = unsigned_integer(); generation && take("R")) {
                        if (text.size() > 10) damaged("an object number out of range");
                        const auto id = std::stoull(std::string{text});
                        if (id == 0 || id > std::numeric_limits<std::uint32_t>::max()) return nullptr;
                        return reference{static_cast<std::uint32_t>(id)};
                    }
                    at_ = back;
                }
                const std::string spelt{text};
                const bool integer_like = !spelt.empty() && spelt.find('.') == std::string::npos;
                char *end = nullptr;
                if (integer_like) {
                    errno = 0;
                    const long long value = std::strtoll(spelt.c_str(), &end, 10);
                    if (end != spelt.c_str() + spelt.size() || spelt == "+" || spelt == "-") damaged("\"" + spelt + "\" isn't a number");
                    if (errno == ERANGE) return std::strtod(spelt.c_str(), nullptr);
                    return integer(value);
                }
                const double value = std::strtod(spelt.c_str(), &end);
                if (end != spelt.c_str() + spelt.size() || spelt == "." || !std::isfinite(value)) damaged("\"" + spelt + "\" isn't a number");
                if (std::abs(value) > 3.4e38) return value > 0 ? 3.4e38 : -3.4e38;
                return value;
            }

            object name_value() {
                ++at_;
                std::string text;
                while (at_ < bytes_.size() && is_regular(bytes_[at_])) {
                    if (bytes_[at_] == '#' && at_ + 2 < bytes_.size() && hex_value(bytes_[at_ + 1]) >= 0 && hex_value(bytes_[at_ + 2]) >= 0) {
                        text += static_cast<char>(hex_value(bytes_[at_ + 1]) * 16 + hex_value(bytes_[at_ + 2]));
                        at_ += 3;
                    } else {
                        text += bytes_[at_++];
                    }
                }
                // "/" alone is a name PDF allows but pdflite's names don't hold; it's read as null.
                if (text.empty() || text.find('\0') != std::string::npos) return nullptr;
                return name{text};
            }

            object literal_string() {
                ++at_;
                std::string out;
                int depth = 1;
                while (true) {
                    if (at_ >= bytes_.size()) damaged("a string that isn't closed");
                    const char c = bytes_[at_++];
                    if (c == '(') {
                        ++depth;
                    } else if (c == ')') {
                        if (--depth == 0) break;
                    } else if (c == '\r') {
                        // An end of line in a string is a line feed, however it's spelt (7.3.4.2).
                        if (at_ < bytes_.size() && bytes_[at_] == '\n') ++at_;
                        out += '\n';
                        continue;
                    } else if (c == '\\') {
                        if (at_ >= bytes_.size()) damaged("a string that isn't closed");
                        const char e = bytes_[at_++];
                        switch (e) {
                            case 'n': out += '\n'; break;
                            case 'r': out += '\r'; break;
                            case 't': out += '\t'; break;
                            case 'b': out += '\b'; break;
                            case 'f': out += '\f'; break;
                            case '\r':
                                if (at_ < bytes_.size() && bytes_[at_] == '\n') ++at_;
                                break; // a line continued
                            case '\n': break;
                            default:
                                if (e >= '0' && e <= '7') {
                                    int code = e - '0';
                                    for (int i = 0; i < 2 && at_ < bytes_.size() && bytes_[at_] >= '0' && bytes_[at_] <= '7'; ++i)
                                        code = code * 8 + (bytes_[at_++] - '0');
                                    out += static_cast<char>(code & 0xFF);
                                } else {
                                    out += e; // \( \) \\ and any other: the character itself
                                }
                        }
                        continue;
                    }
                    out += c;
                }
                return string{std::move(out)};
            }

            object hex_string() {
                ++at_;
                std::string out;
                int high = -1;
                while (true) {
                    if (at_ >= bytes_.size()) damaged("a hex string that isn't closed");
                    const char c = bytes_[at_++];
                    if (c == '>') break;
                    if (is_white(c)) continue;
                    const int digit = hex_value(c);
                    if (digit < 0) damaged("a hex string with '" + std::string(1, c) + "' in it");
                    if (high < 0) {
                        high = digit;
                    } else {
                        out += static_cast<char>(high * 16 + digit);
                        high = -1;
                    }
                }
                if (high >= 0) out += static_cast<char>(high * 16); // an odd last digit is followed by 0
                return string{std::move(out), true};
            }

            object dictionary_value(const int depth) {
                at_ += 2;
                dictionary out;
                while (true) {
                    skip();
                    if (at_ + 1 < bytes_.size() && bytes_[at_] == '>' && bytes_[at_ + 1] == '>') {
                        at_ += 2;
                        return out;
                    }
                    if (at_ >= bytes_.size()) damaged("a dictionary that isn't closed");
                    const object key = value(depth + 1);
                    if (!key.is<name>()) {
                        if (key.is<std::nullptr_t>()) { // "/" alone: its value is read and dropped
                            value(depth + 1);
                            continue;
                        }
                        damaged("a dictionary key that isn't a name, at byte " + std::to_string(at_));
                    }
                    object item = value(depth + 1);
                    // A null value is the same as the entry not being there (7.3.7); a key that comes
                    // again replaces the earlier.
                    if (!item.is<std::nullptr_t>()) out.set(key.as<name>().text(), std::move(item));
                }
            }

            std::string_view bytes_;
            std::size_t at_;
        };

        // ---- Decoding (7.4) ----

        std::string inflate_data(const std::string_view data) {
            z_stream z{};
            if (inflateInit(&z) != Z_OK) damaged("zlib couldn't start");
            std::string out;
            char buffer[65536];
            z.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.data()));
            z.avail_in = static_cast<uInt>(data.size());
            int result = Z_OK;
            while (result == Z_OK) {
                z.next_out = reinterpret_cast<Bytef *>(buffer);
                z.avail_out = sizeof buffer;
                result = inflate(&z, Z_NO_FLUSH);
                out.append(buffer, sizeof buffer - z.avail_out);
                if (result == Z_BUF_ERROR && z.avail_in == 0) break; // data cut short: what came out is kept
            }
            inflateEnd(&z);
            if (result != Z_STREAM_END && result != Z_BUF_ERROR && result != Z_OK) damaged("a FlateDecode stream that zlib can't read");
            return out;
        }

        int integer_in(const dictionary &entries, const std::string_view key, const int fallback) {
            const auto *found = entries.find(key);
            return found && found->is<std::int64_t>() ? static_cast<int>(found->as<std::int64_t>()) : fallback;
        }

        // The predictors of FlateDecode's /DecodeParms (7.4.4.4): PNG's, per row, and TIFF's 2.
        std::string unpredicted(std::string data, const dictionary &parameters) {
            const int predictor = integer_in(parameters, "Predictor", 1);
            if (predictor <= 1) return data;
            const int colours = integer_in(parameters, "Colors", 1);
            const int bits = integer_in(parameters, "BitsPerComponent", 8);
            const int columns = integer_in(parameters, "Columns", 1);
            if (colours < 1 || bits < 1 || columns < 1) damaged("FlateDecode parameters out of range");
            const std::size_t pixel = std::max<std::size_t>(1, static_cast<std::size_t>(colours * bits + 7) / 8);
            const std::size_t row = (static_cast<std::size_t>(colours) * bits * columns + 7) / 8;
            if (predictor == 2) {
                if (bits != 8) damaged("a TIFF predictor with other than 8 bits per component");
                for (std::size_t start = 0; start + row <= data.size(); start += row)
                    for (std::size_t i = pixel; i < row; ++i)
                        data[start + i] = static_cast<char>(static_cast<unsigned char>(data[start + i]) + static_cast<unsigned char>(data[start + i - pixel]));
                return data;
            }
            std::string out;
            std::string previous(row, '\0');
            for (std::size_t start = 0; start + 1 + row <= data.size(); start += row + 1) {
                const int type = static_cast<unsigned char>(data[start]);
                std::string line = data.substr(start + 1, row);
                for (std::size_t i = 0; i < row; ++i) {
                    const int left = i >= pixel ? static_cast<unsigned char>(line[i - pixel]) : 0;
                    const int up = static_cast<unsigned char>(previous[i]);
                    const int up_left = i >= pixel ? static_cast<unsigned char>(previous[i - pixel]) : 0;
                    int add = 0;
                    switch (type) {
                        case 0: break;
                        case 1: add = left; break;
                        case 2: add = up; break;
                        case 3: add = (left + up) / 2; break;
                        case 4: {
                            const int p = left + up - up_left;
                            const int pa = std::abs(p - left), pb = std::abs(p - up), pc = std::abs(p - up_left);
                            add = pa <= pb && pa <= pc ? left : pb <= pc ? up : up_left;
                            break;
                        }
                        default: damaged("a PNG predictor row of unknown type " + std::to_string(type));
                    }
                    line[i] = static_cast<char>(static_cast<unsigned char>(line[i]) + add);
                }
                out += line;
                previous = std::move(line);
            }
            return out;
        }

        // ---- The standard security handler, revisions 2 to 4 (7.6.3) ----

        std::string md5(const std::string_view data) {
            unsigned char out[EVP_MAX_MD_SIZE];
            unsigned int size = 0;
            if (EVP_Digest(data.data(), data.size(), out, &size, EVP_md5(), nullptr) != 1) damaged("OpenSSL couldn't hash with MD5");
            return {reinterpret_cast<char *>(out), size};
        }

        // RC4 (7.6.2): ours, as OpenSSL 3 only has it in its legacy provider. It's only ever used to
        // read old files.
        std::string rc4(const std::string_view key, const std::string_view data) {
            std::array<unsigned char, 256> s{};
            for (int i = 0; i < 256; ++i) s[i] = static_cast<unsigned char>(i);
            for (int i = 0, j = 0; i < 256; ++i) {
                j = (j + s[i] + static_cast<unsigned char>(key[i % key.size()])) & 0xFF;
                std::swap(s[i], s[j]);
            }
            std::string out(data);
            for (std::size_t n = 0, i = 0, j = 0; n < out.size(); ++n) {
                i = (i + 1) & 0xFF;
                j = (j + s[i]) & 0xFF;
                std::swap(s[i], s[j]);
                out[n] = static_cast<char>(static_cast<unsigned char>(out[n]) ^ s[(s[i] + s[j]) & 0xFF]);
            }
            return out;
        }

        std::string aes128_decrypt(const std::string_view key, const std::string_view data) {
            if (data.size() < 32 || data.size() % 16 != 0) return {}; // an IV and at least one block, or nothing
            const std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> context{EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free};
            if (!context || EVP_DecryptInit_ex(context.get(), EVP_aes_128_cbc(), nullptr, reinterpret_cast<const unsigned char *>(key.data()),
                                               reinterpret_cast<const unsigned char *>(data.data())) != 1)
                damaged("OpenSSL couldn't start AES");
            std::string out(data.size(), '\0');
            int written = 0, last = 0;
            if (EVP_DecryptUpdate(context.get(), reinterpret_cast<unsigned char *>(out.data()), &written,
                                  reinterpret_cast<const unsigned char *>(data.data()) + 16, static_cast<int>(data.size() - 16)) != 1 ||
                EVP_DecryptFinal_ex(context.get(), reinterpret_cast<unsigned char *>(out.data()) + written, &last) != 1)
                return {};
            out.resize(static_cast<std::size_t>(written + last));
            return out;
        }

        // The 32 bytes a password is padded with (7.6.3.3, algorithm 2).
        const std::string padding{"\x28\xBF\x4E\x5E\x4E\x75\x8A\x41\x64\x00\x4E\x56\xFF\xFA\x01\x08"
                                  "\x2E\x2E\x00\xB6\xD0\x68\x3E\x80\x2F\x0C\xA9\xFE\x64\x53\x69\x7A", 32};

        std::string padded(const std::string_view password) { return (std::string{password.substr(0, 32)} + padding).substr(0, 32); }

        enum class method { none, rc4, aes128, aes256 };

        // What decrypting a file takes: its key and how strings and streams are encrypted.
        struct security {
            std::uint32_t encrypt_id = 0; // the /Encrypt dictionary's object, never encrypted itself
            int revision = 0;
            std::string key;
            method strings = method::none;
            method streams = method::none;
            bool metadata = true; // EncryptMetadata

            // An object's string or stream decrypted (algorithm 1 for revisions 2 to 4).
            [[nodiscard]] std::string decrypt(const std::string_view data, const method how, const std::uint32_t id, const std::uint32_t generation) const {
                if (how == method::none) return std::string{data};
                if (how == method::aes256) {
                    try {
                        return encryption::decrypt(key, data);
                    } catch (const std::invalid_argument &) {
                        return {};
                    }
                }
                std::string material = key;
                material += static_cast<char>(id & 0xFF);
                material += static_cast<char>((id >> 8) & 0xFF);
                material += static_cast<char>((id >> 16) & 0xFF);
                material += static_cast<char>(generation & 0xFF);
                material += static_cast<char>((generation >> 8) & 0xFF);
                if (how == method::aes128) material += "sAlT";
                const std::string object_key = md5(material).substr(0, std::min<std::size_t>(key.size() + 5, 16));
                return how == method::rc4 ? rc4(object_key, data) : aes128_decrypt(object_key, data);
            }
        };

        std::string string_in(const dictionary &entries, const std::string_view key) {
            const auto *found = entries.find(key);
            return found && found->is<string>() ? found->as<string>().bytes() : std::string{};
        }

        // Algorithm 2: the file key from a (padded) password.
        std::string legacy_key(const std::string &padded_password, const dictionary &encrypt, const std::string &first_id, const int revision,
                               const std::size_t length, const bool metadata) {
            const auto p = static_cast<std::uint32_t>(integer_in(encrypt, "P", 0));
            std::string input = padded_password + string_in(encrypt, "O").substr(0, 32);
            for (int i = 0; i < 4; ++i) input += static_cast<char>((p >> (8 * i)) & 0xFF);
            input += first_id;
            if (revision >= 4 && !metadata) input += std::string(4, '\xFF');
            std::string hash = md5(input);
            if (revision >= 3)
                for (int i = 0; i < 50; ++i) hash = md5(hash.substr(0, length));
            return hash.substr(0, length);
        }

        // Algorithm 6: whether key is the one the user password gives.
        bool user_key_matches(const std::string &key, const dictionary &encrypt, const std::string &first_id, const int revision) {
            const std::string u = string_in(encrypt, "U");
            if (revision == 2) return rc4(key, padding) == u.substr(0, 32);
            std::string x = rc4(key, md5(padding + first_id));
            for (int i = 1; i <= 19; ++i) {
                std::string round_key = key;
                for (auto &c: round_key) c = static_cast<char>(c ^ i);
                x = rc4(round_key, x);
            }
            return u.size() >= 16 && x.substr(0, 16) == u.substr(0, 16);
        }

        // Algorithm 7: the (padded) user password an owner password unlocks from /O.
        std::string user_password_from_owner(const std::string_view owner, const dictionary &encrypt, const int revision, const std::size_t length) {
            std::string hash = md5(padded(owner));
            if (revision >= 3)
                for (int i = 0; i < 50; ++i) hash = md5(hash);
            const std::string key = hash.substr(0, length);
            std::string x = string_in(encrypt, "O").substr(0, 32);
            if (revision == 2) return rc4(key, x);
            for (int i = 19; i >= 0; --i) {
                std::string round_key = key;
                for (auto &c: round_key) c = static_cast<char>(c ^ i);
                x = rc4(round_key, x);
            }
            return x;
        }

        method crypt_method(const dictionary &encrypt, const std::string_view which, const int version) {
            if (version < 4) return method::rc4;
            const auto *filter = encrypt.find(which);
            const std::string chosen = filter && filter->is<name>() ? filter->as<name>().text() : "Identity";
            if (chosen == "Identity") return method::none;
            const auto *filters = encrypt.find("CF");
            if (!filters || !filters->is<dictionary>()) damaged("an encryption dictionary without its crypt filters");
            const auto *entry = filters->as<dictionary>().find(chosen);
            if (!entry || !entry->is<dictionary>()) damaged("crypt filter " + chosen + " isn't defined");
            const auto *cfm = entry->as<dictionary>().find("CFM");
            const std::string kind = cfm && cfm->is<name>() ? cfm->as<name>().text() : "None";
            if (kind == "V2") return method::rc4;
            if (kind == "AESV2") return method::aes128;
            if (kind == "AESV3") return method::aes256;
            if (kind == "None") return method::none;
            damaged("crypt filter method " + kind + " isn't supported");
        }
    }

    // ---- The reader ----

    struct reader::impl {
        // Where an object is (7.5.4, 7.5.8.3).
        struct entry {
            enum class kind { free, at, compressed } type = kind::free;
            std::uint64_t offset = 0;     // at: where it starts; compressed: its object stream
            std::uint32_t generation = 0; // at: its generation; compressed: its index there
        };

        std::string bytes;
        std::size_t base = 0; // where "%PDF-" starts: offsets count from there
        std::vector<entry> table;
        dictionary trailer_entries;
        std::optional<pdflite::security> security;
        std::vector<std::uint32_t> page_ids;
        std::vector<dictionary> inherited; // per page: what it inherits

        mutable std::unordered_map<std::uint32_t, object> objects;
        mutable std::unordered_map<std::uint32_t, pdflite::stream> streams;
        mutable std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> object_stream_offsets; // per object stream
        mutable std::unordered_map<std::uint32_t, std::string> object_stream_data;
        mutable std::set<std::uint32_t> reading; // objects being read: a /Length that refers back to its own stream

        // The entries already set by a newer section win (7.5.6); this marks them.
        std::vector<bool> seen;

        void note(const std::uint32_t id, const entry &where) {
            if (id >= seen.size()) seen.resize(id + 1, false);
            if (seen[id]) return;
            if (id >= table.size()) {
                if (id > 10'000'000) damaged("an object number out of range");
                table.resize(id + 1);
            }
            table[id] = where;
            seen[id] = true;
        }

        void read_structure() {
            // The header (7.5.2), anywhere in the first kilobyte: offsets count from it.
            const auto header = std::string_view{bytes}.substr(0, 1024).find("%PDF-");
            if (header == std::string_view::npos) damaged("not a PDF (no %PDF- header)");
            base = header;
            // startxref, in the last kilobyte or so (7.5.5).
            const std::size_t tail = bytes.size() > 2048 ? bytes.size() - 2048 : 0;
            const auto keyword = std::string_view{bytes}.rfind("startxref");
            if (keyword == std::string_view::npos || keyword < tail) damaged("no startxref: the file is cut short or damaged");
            parser at{bytes, keyword + 9};
            const auto start = at.unsigned_integer();
            if (!start) damaged("startxref isn't followed by an offset");
            std::set<std::uint64_t> visited;
            std::optional<std::uint64_t> section = *start;
            bool newest = true;
            while (section) {
                if (!visited.insert(*section).second) break; // a /Prev that loops
                const auto previous = read_section(*section, newest);
                newest = false;
                section = previous;
            }
            if (!trailer_entries.contains("Root")) damaged("the trailer has no /Root");
        }

        // One cross-reference section and its trailer: a table or a stream. Its /Prev, if any.
        std::optional<std::uint64_t> read_section(const std::uint64_t offset, const bool newest) {
            const std::size_t at = base + offset;
            if (at >= bytes.size()) damaged("a cross-reference offset beyond the end of the file");
            parser p{bytes, at};
            dictionary section_trailer;
            if (p.take("xref")) {
                while (true) {
                    const auto first = p.unsigned_integer();
                    if (!first) break;
                    const auto count = p.unsigned_integer();
                    if (!count) damaged("a cross-reference subsection without its count");
                    for (std::uint64_t i = 0; i < *count; ++i) {
                        const auto where = p.unsigned_integer();
                        const auto generation = p.unsigned_integer();
                        const auto type = p.word();
                        if (!where || !generation || (type != "n" && type != "f")) damaged("a damaged cross-reference table");
                        const auto id = *first + i;
                        if (id > std::numeric_limits<std::uint32_t>::max()) damaged("an object number out of range");
                        entry e;
                        if (type == "n") {
                            e.type = entry::kind::at;
                            e.offset = *where;
                            e.generation = static_cast<std::uint32_t>(*generation);
                        }
                        note(static_cast<std::uint32_t>(id), e);
                    }
                }
                p.expect("trailer");
                const object read = p.value();
                if (!read.is<dictionary>()) damaged("the trailer isn't a dictionary");
                section_trailer = read.as<dictionary>();
                // A hybrid file's cross-reference stream (7.5.8.4): its entries come after the table's.
                if (const auto *stream_offset = section_trailer.find("XRefStm"); stream_offset && stream_offset->is<std::int64_t>())
                    read_stream_section(static_cast<std::uint64_t>(stream_offset->as<std::int64_t>()));
            } else {
                section_trailer = read_stream_section(offset);
            }
            if (newest) trailer_entries = section_trailer;
            if (const auto *prev = section_trailer.find("Prev"); prev && prev->is<std::int64_t>() && prev->as<std::int64_t>() >= 0)
                return static_cast<std::uint64_t>(prev->as<std::int64_t>());
            return std::nullopt;
        }

        // A cross-reference stream (7.5.8): its entries noted, its dictionary returned.
        dictionary read_stream_section(const std::uint64_t offset) {
            const auto [unused, value, data] = indirect_at(base + offset, 0, false);
            if (!value.is<dictionary>() || !data) damaged("a cross-reference offset that leads to neither a table nor a stream");
            const auto &entries = value.as<dictionary>();
            const auto *type = entries.find("Type");
            if (!type || !type->is<name>() || type->as<name>().text() != "XRef") damaged("a cross-reference stream without /Type /XRef");
            const std::string decoded_data = decode(entries, *data);
            const auto *widths = entries.find("W");
            if (!widths || !widths->is<array>() || widths->as<array>().size() != 3) damaged("a cross-reference stream without /W");
            std::array<std::size_t, 3> w{};
            for (std::size_t i = 0; i < 3; ++i) {
                const auto &item = widths->as<array>()[i];
                if (!item.is<std::int64_t>() || item.as<std::int64_t>() < 0 || item.as<std::int64_t>() > 8) damaged("a cross-reference stream's /W out of range");
                w[i] = static_cast<std::size_t>(item.as<std::int64_t>());
            }
            const std::size_t row = w[0] + w[1] + w[2];
            if (row == 0) damaged("a cross-reference stream's /W out of range");
            std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
            if (const auto *index = entries.find("Index"); index && index->is<array>()) {
                const auto &items = index->as<array>();
                for (std::size_t i = 0; i + 1 < items.size(); i += 2)
                    if (items[i].is<std::int64_t>() && items[i + 1].is<std::int64_t>())
                        ranges.emplace_back(items[i].as<std::int64_t>(), items[i + 1].as<std::int64_t>());
            } else {
                ranges.emplace_back(0, static_cast<std::uint64_t>(integer_in(entries, "Size", 0)));
            }
            auto field = [&](const std::size_t at, const std::size_t width, const std::uint64_t fallback) {
                if (width == 0) return fallback;
                std::uint64_t out = 0;
                for (std::size_t i = 0; i < width; ++i) out = out << 8 | static_cast<unsigned char>(decoded_data[at + i]);
                return out;
            };
            std::size_t at = 0;
            for (const auto &[first, count]: ranges) {
                for (std::uint64_t i = 0; i < count; ++i, at += row) {
                    if (at + row > decoded_data.size()) damaged("a cross-reference stream shorter than its /Index says");
                    const auto object_id = first + i;
                    if (object_id > std::numeric_limits<std::uint32_t>::max()) damaged("an object number out of range");
                    const auto type = field(at, w[0], 1);
                    const auto second = field(at + w[0], w[1], 0);
                    const auto third = field(at + w[0] + w[1], w[2], 0);
                    entry e;
                    if (type == 1) {
                        e.type = entry::kind::at;
                        e.offset = second;
                        e.generation = static_cast<std::uint32_t>(third);
                    } else if (type == 2) {
                        e.type = entry::kind::compressed;
                        e.offset = second;
                        e.generation = static_cast<std::uint32_t>(third);
                    } else if (type != 0) {
                        continue; // unknown types are references to null (7.5.8.3)
                    }
                    note(static_cast<std::uint32_t>(object_id), e);
                }
            }
            return entries;
        }

        struct indirect {
            std::uint32_t id;
            object value;
            std::optional<std::string> stream_data; // raw, as in the file
        };

        // The indirect object at a position: "12 0 obj ... endobj", a stream's data with it.
        // expected (when not 0) is the number it should have.
        indirect indirect_at(const std::size_t at, const std::uint32_t expected, const bool decrypting) const {
            if (at >= bytes.size()) damaged("an object offset beyond the end of the file");
            parser p{bytes, at};
            const auto id = p.unsigned_integer();
            const auto generation = p.unsigned_integer();
            if (!id || !generation || !p.take("obj"))
                damaged("the cross-reference doesn't lead to object " + std::to_string(expected) + " (byte " + std::to_string(at) + ")");
            if (expected != 0 && *id != expected)
                damaged("the cross-reference leads to object " + std::to_string(*id) + " where " + std::to_string(expected) + " should be");
            object value = p.value();
            std::optional<std::string> data;
            if (value.is<dictionary>() && p.peek_word() == "stream") {
                // The data starts after "stream" and its end of line: CR LF or LF (a lone CR is
                // taken too).
                std::size_t start = p.position() + 6;
                if (start < bytes.size() && bytes[start] == '\r') ++start;
                if (start < bytes.size() && bytes[start] == '\n') ++start;
                const auto *length = value.as<dictionary>().find("Length");
                std::optional<std::uint64_t> size;
                if (length && length->is<std::int64_t>() && length->as<std::int64_t>() >= 0) {
                    size = static_cast<std::uint64_t>(length->as<std::int64_t>());
                } else if (length && length->is<reference>() && !reading.contains(length->as<reference>().id)) {
                    if (const object resolved = get(length->as<reference>().id); resolved.is<std::int64_t>() && resolved.as<std::int64_t>() >= 0)
                        size = static_cast<std::uint64_t>(resolved.as<std::int64_t>());
                }
                // Checked: "endstream" right after (white space allowed); else found by looking.
                auto ends_right = [&](const std::uint64_t n) {
                    if (start + n > bytes.size()) return false;
                    parser after{bytes, static_cast<std::size_t>(start + n)};
                    return after.peek_word() == "endstream";
                };
                if (!size || !ends_right(*size)) {
                    const auto end = bytes.find("endstream", start);
                    if (end == std::string::npos) damaged("a stream without endstream");
                    std::size_t stop = end;
                    if (stop > start && bytes[stop - 1] == '\n') --stop;
                    if (stop > start && bytes[stop - 1] == '\r') --stop;
                    size = stop - start;
                }
                data = bytes.substr(start, *size);
            }
            if (decrypting && security && *id != security->encrypt_id) {
                const auto gen = static_cast<std::uint32_t>(*generation);
                value = decrypted(value, static_cast<std::uint32_t>(*id), gen);
                if (data) {
                    const auto &entries = value.as<dictionary>();
                    const auto *type = entries.find("Type");
                    const std::string kind = type && type->is<name>() ? type->as<name>().text() : "";
                    if (kind != "XRef" && !(kind == "Metadata" && !security->metadata))
                        data = security->decrypt(*data, security->streams, static_cast<std::uint32_t>(*id), gen);
                }
            }
            return {static_cast<std::uint32_t>(*id), std::move(value), std::move(data)};
        }

        object decrypted(const object &value, const std::uint32_t id, const std::uint32_t generation) const {
            if (value.is<string>())
                return string{security->decrypt(value.as<string>().bytes(), security->strings, id, generation), value.as<string>().hex()};
            if (value.is<array>()) {
                array out;
                for (const auto &item: value.as<array>()) out.push_back(decrypted(item, id, generation));
                return out;
            }
            if (value.is<dictionary>()) {
                dictionary out;
                for (const auto &[key, item]: value.as<dictionary>()) out.add(key.text(), decrypted(item, id, generation));
                return out;
            }
            return value;
        }

        object get(const std::uint32_t id) const {
            if (const auto found = objects.find(id); found != objects.end()) return found->second;
            if (id >= table.size() || table[id].type == entry::kind::free) return nullptr;
            if (reading.contains(id)) damaged("object " + std::to_string(id) + " refers back to itself while being read");
            reading.insert(id);
            struct done {
                std::set<std::uint32_t> &set;
                std::uint32_t id;
                ~done() { set.erase(id); }
            } finally{reading, id};
            const entry &where = table[id];
            if (where.type == entry::kind::at) {
                auto read = indirect_at(base + where.offset, id, true);
                if (read.stream_data) streams.emplace(id, pdflite::stream{read.value.as<dictionary>(), std::move(*read.stream_data)});
                return objects.emplace(id, std::move(read.value)).first->second;
            }
            return objects.emplace(id, compressed(id, static_cast<std::uint32_t>(where.offset), where.generation)).first->second;
        }

        // An object kept in an object stream (7.5.7): its strings aren't encrypted again.
        object compressed(const std::uint32_t id, const std::uint32_t container, const std::uint32_t index) const {
            if (!object_stream_offsets.contains(container)) {
                get(container);
                const auto found = streams.find(container);
                if (found == streams.end()) damaged("object " + std::to_string(id) + "'s object stream isn't a stream");
                const auto &entries = found->second.entries;
                std::string data = decode(entries, found->second.data);
                const int count = integer_in(entries, "N", -1);
                const int first = integer_in(entries, "First", -1);
                if (count < 0 || first < 0 || static_cast<std::size_t>(first) > data.size()) damaged("an object stream without /N or /First");
                parser p{data, 0};
                std::vector<std::uint32_t> offsets;
                for (int i = 0; i < count; ++i) {
                    const auto number = p.unsigned_integer();
                    const auto offset = p.unsigned_integer();
                    if (!number || !offset) damaged("an object stream's header is damaged");
                    offsets.push_back(static_cast<std::uint32_t>(first + *offset));
                }
                object_stream_offsets.emplace(container, std::move(offsets));
                object_stream_data.emplace(container, std::move(data));
            }
            const auto &offsets = object_stream_offsets.at(container);
            if (index >= offsets.size()) damaged("object " + std::to_string(id) + " isn't where its object stream should have it");
            parser p{object_stream_data.at(container), offsets[index]};
            return p.value();
        }

        object resolve(const object &value) const { return value.is<reference>() ? get(value.as<reference>().id) : value; }

        std::string decode(const dictionary &entries, const std::string &data) const {
            const object filters = resolve(entries.find("Filter") ? *entries.find("Filter") : object{});
            const object parameters = resolve(entries.find("DecodeParms") ? *entries.find("DecodeParms") : object{});
            std::vector<std::string> names;
            std::vector<dictionary> settings;
            if (filters.is<name>()) {
                names.push_back(filters.as<name>().text());
                settings.push_back(parameters.is<dictionary>() ? parameters.as<dictionary>() : dictionary{});
            } else if (filters.is<array>()) {
                for (std::size_t i = 0; i < filters.as<array>().size(); ++i) {
                    const object each = resolve(filters.as<array>()[i]);
                    if (!each.is<name>()) damaged("a /Filter that isn't a name");
                    names.push_back(each.as<name>().text());
                    dictionary these;
                    if (parameters.is<array>() && i < parameters.as<array>().size()) {
                        if (const object p = resolve(parameters.as<array>()[i]); p.is<dictionary>()) these = p.as<dictionary>();
                    }
                    settings.push_back(std::move(these));
                }
            }
            std::string out = data;
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (names[i] == "FlateDecode" || names[i] == "Fl") {
                    out = unpredicted(inflate_data(out), settings[i]);
                } else if (names[i] != "Crypt") {
                    damaged("the " + names[i] + " filter isn't one the reader decodes");
                }
            }
            return out;
        }

        void read_security(const std::string_view password) {
            const auto *encrypt_entry = trailer_entries.find("Encrypt");
            if (!encrypt_entry) return;
            pdflite::security s;
            object encrypt_value = *encrypt_entry;
            if (encrypt_value.is<reference>()) {
                s.encrypt_id = encrypt_value.as<reference>().id;
                encrypt_value = get(s.encrypt_id); // read before security is set: never decrypted
            }
            if (!encrypt_value.is<dictionary>()) damaged("the trailer's /Encrypt isn't a dictionary");
            const auto &encrypt = encrypt_value.as<dictionary>();
            const auto *filter = encrypt.find("Filter");
            if (!filter || !filter->is<name>() || filter->as<name>().text() != "Standard")
                damaged("encrypted by a security handler other than the standard one");
            const int version = integer_in(encrypt, "V", 0);
            s.revision = integer_in(encrypt, "R", 0);
            if (const auto *metadata = encrypt.find("EncryptMetadata"); metadata && metadata->is<bool>()) s.metadata = metadata->as<bool>();
            if (s.revision == 6) {
                auto key = encryption::key_for(encrypt, password);
                if (!key && !password.empty()) key = encryption::key_for(encrypt, "");
                if (!key) damaged("the password doesn't open this file");
                s.key = std::move(*key);
                s.strings = crypt_method(encrypt, "StrF", version);
                s.streams = crypt_method(encrypt, "StmF", version);
            } else if (s.revision >= 2 && s.revision <= 4) {
                std::string first_id;
                if (const auto *ids = trailer_entries.find("ID"); ids && ids->is<array>() && !ids->as<array>().empty() && ids->as<array>()[0].is<string>())
                    first_id = ids->as<array>()[0].as<string>().bytes();
                std::size_t length = 5;
                if (s.revision >= 3) {
                    length = static_cast<std::size_t>(integer_in(encrypt, "Length", 40) / 8);
                    if (version == 4) length = 16;
                    if (length < 5 || length > 16) damaged("an encryption key length out of range");
                }
                // The password as the user's, as the owner's, then no password at all.
                std::optional<std::string> key;
                for (const auto &candidate: {padded(password), user_password_from_owner(password, encrypt, s.revision, length), padded("")}) {
                    const std::string k = legacy_key(candidate, encrypt, first_id, s.revision, length, s.metadata);
                    if (user_key_matches(k, encrypt, first_id, s.revision)) {
                        key = k;
                        break;
                    }
                }
                if (!key) damaged("the password doesn't open this file");
                s.key = std::move(*key);
                s.strings = crypt_method(encrypt, "StrF", version);
                s.streams = crypt_method(encrypt, "StmF", version);
            } else {
                damaged("encryption revision " + std::to_string(s.revision) + " isn't supported");
            }
            security = std::move(s);
            objects.clear(); // read before the key was known
            streams.clear();
        }

        // The page tree (7.7.3), what each page inherits carried down.
        void read_pages() {
            const object root = resolve(trailer_entries.at("Root"));
            if (!root.is<dictionary>()) damaged("the catalog isn't a dictionary");
            const auto *pages = root.as<dictionary>().find("Pages");
            if (!pages || !pages->is<reference>()) damaged("the catalog has no page tree");
            std::set<std::uint32_t> visited;
            walk(pages->as<reference>().id, {}, visited, 0);
        }

        void walk(const std::uint32_t id, dictionary inherits, std::set<std::uint32_t> &visited, const int depth) {
            if (depth > 64 || !visited.insert(id).second) return; // a tree that loops
            const object node = get(id);
            if (!node.is<dictionary>()) return;
            const auto &entries = node.as<dictionary>();
            for (const auto key: {"Resources", "MediaBox", "CropBox", "Rotate"})
                if (const auto *value = entries.find(key)) inherits.set(key, *value);
            const auto *kids = entries.find("Kids");
            const auto *type = entries.find("Type");
            const bool tree = (type && type->is<name>() && type->as<name>().text() == "Pages") || (kids && !(type && type->is<name>() && type->as<name>().text() == "Page"));
            if (!tree) {
                page_ids.push_back(id);
                inherited.push_back(std::move(inherits));
                return;
            }
            const object list = kids ? resolve(*kids) : object{};
            if (!list.is<array>()) return;
            for (const auto &kid: list.as<array>())
                if (kid.is<reference>()) walk(kid.as<reference>().id, inherits, visited, depth + 1);
        }
    };

    reader::reader(std::shared_ptr<impl> state) : impl_(std::move(state)) {}

    reader::reader(const std::filesystem::path &file, const std::string_view password) {
        std::ifstream in{file, std::ios::binary};
        if (!in) throw std::runtime_error("sc::pdflite: can't read " + file.string());
        std::string bytes{std::istreambuf_iterator<char>{in}, {}};
        if (in.bad()) throw std::runtime_error("sc::pdflite: can't read " + file.string());
        impl_ = opened(std::move(bytes), password);
    }

    reader reader::from_memory(std::string bytes, const std::string_view password) { return reader{opened(std::move(bytes), password)}; }

    std::shared_ptr<reader::impl> reader::opened(std::string bytes, const std::string_view password) {
        auto state = std::make_shared<impl>();
        state->bytes = std::move(bytes);
        state->read_structure();
        state->read_security(password);
        state->read_pages();
        return state;
    }

    const dictionary &reader::trailer() const { return impl_->trailer_entries; }

    bool reader::encrypted() const { return impl_->security.has_value(); }

    object reader::get(const std::uint32_t id) const { return impl_->get(id); }

    bool reader::is_stream(const std::uint32_t id) const {
        impl_->get(id);
        return impl_->streams.contains(id);
    }

    std::optional<pdflite::stream> reader::stream(const std::uint32_t id) const {
        impl_->get(id);
        const auto found = impl_->streams.find(id);
        if (found == impl_->streams.end()) return std::nullopt;
        return found->second;
    }

    object reader::resolve(const object &value) const { return impl_->resolve(value); }

    std::string reader::decoded(const pdflite::stream &encoded) const { return impl_->decode(encoded.entries, encoded.data); }

    const std::vector<std::uint32_t> &reader::pages() const { return impl_->page_ids; }

    dictionary reader::page(const std::size_t index) const {
        if (index >= impl_->page_ids.size()) throw std::out_of_range("sc::pdflite: page " + std::to_string(index) + " of " + std::to_string(impl_->page_ids.size()));
        const object value = impl_->get(impl_->page_ids[index]);
        dictionary out = value.as<dictionary>();
        for (const auto &[key, item]: impl_->inherited[index])
            if (!out.contains(key.text())) out.add(key.text(), item);
        return out;
    }

    dictionary reader::catalog() const {
        const object root = impl_->resolve(impl_->trailer_entries.at("Root"));
        return root.as<dictionary>();
    }
}
