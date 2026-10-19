#include "pdflite.h"
#include <zlib.h>

#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <random>
#include <stdexcept>

namespace sc::pdflite {
    std::string number(const double value) {
        if (!std::isfinite(value)) throw std::invalid_argument("sc::pdflite: a number that isn't finite");
        if (std::fabs(value) > 3.403e38) throw std::invalid_argument("sc::pdflite: a number too large for PDF readers");
        const double rounded = std::round(value * 10000) / 10000;
        if (rounded == 0) return "0"; // never "-0"
        if (std::trunc(rounded) == rounded && std::fabs(rounded) <= 2147483647.0)
            return std::to_string(static_cast<long long>(rounded));
        std::array<char, 64> buffer{};
        const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), rounded, std::chars_format::fixed, 4);
        if (error != std::errc{}) throw std::invalid_argument("sc::pdflite: a number that can't be written");
        std::string out{buffer.data(), end};
        while (out.back() == '0') out.pop_back();
        if (out.back() == '.') out += '0'; // a whole number beyond 32 bits stays a real
        return out;
    }

    std::string name(const std::string_view text) {
        if (text.empty()) throw std::invalid_argument("sc::pdflite: an empty name");
        static constexpr std::string_view delimiters = "()<>[]{}/%#";
        std::string out = "/";
        for (const char c: text) {
            const auto byte = static_cast<unsigned char>(c);
            if (byte == 0) throw std::invalid_argument("sc::pdflite: a name with a zero byte");
            if (byte < '!' || byte > '~' || delimiters.find(c) != std::string_view::npos) {
                char escaped[4];
                std::snprintf(escaped, sizeof escaped, "#%02X", byte);
                out += escaped;
            } else out += c;
        }
        return out;
    }

    std::string literal(const std::string_view bytes) {
        std::string out = "(";
        for (const char c: bytes) {
            const auto byte = static_cast<unsigned char>(c);
            if (c == '(' || c == ')' || c == '\\') {
                out += '\\';
                out += c;
            } else if (byte < ' ' || byte > '~') {
                char escaped[5];
                std::snprintf(escaped, sizeof escaped, "\\%03o", byte);
                out += escaped;
            } else out += c;
        }
        return out + ')';
    }

    std::string hex(const std::string_view bytes) {
        static constexpr char digits[] = "0123456789ABCDEF";
        std::string out = "<";
        for (const char c: bytes) {
            const auto byte = static_cast<unsigned char>(c);
            out += digits[byte >> 4];
            out += digits[byte & 15];
        }
        return out + '>';
    }

    std::u32string code_points(const std::string_view utf8) {
        std::u32string out;
        for (std::size_t i = 0; i < utf8.size();) {
            const auto first = static_cast<unsigned char>(utf8[i]);
            const std::size_t length = first < 0x80 ? 1 : (first >> 5) == 6 ? 2 : (first >> 4) == 14 ? 3 : (first >> 3) == 30 ? 4 : 0;
            if (length == 0 || i + length > utf8.size()) throw std::invalid_argument("sc::pdflite: text that isn't UTF-8");
            char32_t code = length == 1 ? first : first & (0x7F >> length);
            for (std::size_t k = 1; k < length; ++k) {
                const auto next = static_cast<unsigned char>(utf8[i + k]);
                if ((next >> 6) != 2) throw std::invalid_argument("sc::pdflite: text that isn't UTF-8");
                code = (code << 6) | (next & 0x3F);
            }
            if (code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) throw std::invalid_argument("sc::pdflite: text that isn't UTF-8");
            out += code;
            i += length;
        }
        return out;
    }

    std::string text_string(const std::string_view utf8) {
        const auto codes = code_points(utf8);
        bool ascii = true;
        for (const char32_t code: codes)
            if (code < ' ' || code > '~') ascii = false;
        if (ascii) return literal(utf8);
        std::string utf16 = "\xFE\xFF"; // UTF-16BE's byte order mark
        const auto unit = [&utf16](const char32_t value) {
            utf16 += static_cast<char>(value >> 8);
            utf16 += static_cast<char>(value & 0xFF);
        };
        for (const char32_t code: codes) {
            if (code < 0x10000) unit(code);
            else {
                unit(0xD800 + ((code - 0x10000) >> 10));
                unit(0xDC00 + ((code - 0x10000) & 0x3FF));
            }
        }
        return hex(utf16);
    }

    std::string date(const std::chrono::system_clock::time_point time) {
        const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
        std::tm local{};
        localtime_r(&seconds, &local);
        char stamp[16];
        std::strftime(stamp, sizeof stamp, "%Y%m%d%H%M%S", &local);
        const long offset = local.tm_gmtoff / 60; // minutes east of UTC
        char zone[12];
        if (offset == 0) std::snprintf(zone, sizeof zone, "Z");
        else std::snprintf(zone, sizeof zone, "%c%02ld'%02ld'", offset < 0 ? '-' : '+', std::labs(offset) / 60, std::labs(offset) % 60);
        return std::string{"(D:"} + stamp + zone + ")";
    }

    std::string reference(const std::uint32_t id) { return std::to_string(id) + " 0 R"; }

    std::string deflate(const std::string_view data) {
        uLongf size = compressBound(static_cast<uLong>(data.size()));
        std::string out(size, '\0');
        if (compress2(reinterpret_cast<Bytef *>(out.data()), &size, reinterpret_cast<const Bytef *>(data.data()),
                      static_cast<uLong>(data.size()), Z_DEFAULT_COMPRESSION) != Z_OK)
            throw std::runtime_error("sc::pdflite: zlib couldn't compress a stream");
        out.resize(size);
        return out;
    }

    file::file(const std::filesystem::path &path) : path_(path), out_(path, std::ios::binary | std::ios::trunc) {
        if (!out_) throw std::runtime_error("sc::pdflite: can't create " + path.string());
        // The version, then a comment of bytes above 127, so transfers treat the file as binary (7.5.2).
        write("%PDF-1.7\n%\xE2\xE3\xCF\xD3\n");
    }

    std::uint32_t file::allocate() {
        offsets_.push_back(0);
        return static_cast<std::uint32_t>(offsets_.size());
    }

    void file::write(const std::string_view bytes) {
        out_.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        offset_ += bytes.size();
    }

    void file::object(const std::uint32_t id, const std::string_view body) {
        if (finished_) throw std::logic_error("sc::pdflite: writing an object after the file was finished");
        if (id == 0 || id > offsets_.size()) throw std::logic_error("sc::pdflite: an object number that wasn't allocated");
        if (offsets_[id - 1] != 0) throw std::logic_error("sc::pdflite: object " + std::to_string(id) + " written twice");
        offsets_[id - 1] = offset_;
        write(std::to_string(id) + " 0 obj\n");
        write(body);
        write("\nendobj\n");
    }

    void file::stream(const std::uint32_t id, const std::string_view dictionary, const std::string_view data, const bool compress) {
        const std::string packed = compress ? deflate(data) : std::string{data};
        std::string head = "<<";
        if (!dictionary.empty()) head.append(" ").append(dictionary);
        head += " /Length " + std::to_string(packed.size());
        if (compress) head += " /Filter /FlateDecode";
        head += " >>\nstream\n"; // the end of line after "stream" is a line feed, never a lone return
        object(id, head + packed + "\nendstream");
    }

    void file::finish(const std::uint32_t root, const std::uint32_t info) {
        if (finished_) return;
        for (std::size_t i = 0; i < offsets_.size(); ++i)
            if (offsets_[i] == 0) throw std::logic_error("sc::pdflite: object " + std::to_string(i + 1) + " was never written");
        const std::uint64_t table = offset_;
        // Every entry is exactly 20 bytes, ending in a space and a line feed (7.5.4).
        std::string xref = "xref\n0 " + std::to_string(offsets_.size() + 1) + "\n0000000000 65535 f \n";
        for (const auto offset: offsets_) {
            char entry[21];
            std::snprintf(entry, sizeof entry, "%010llu 00000 n \n", static_cast<unsigned long long>(offset));
            xref += entry;
        }
        write(xref);
        // The file's identifier (14.4), random: both halves the same, as in a file never updated.
        std::random_device random;
        std::string id;
        for (int i = 0; i < 16; ++i) id += static_cast<char>(random() & 0xFF);
        std::string trailer = "trailer\n<< /Size " + std::to_string(offsets_.size() + 1) + " /Root " + reference(root);
        if (info != 0) trailer += " /Info " + reference(info);
        trailer += " /ID [" + hex(id) + " " + hex(id) + "] >>\nstartxref\n" + std::to_string(table) + "\n%%EOF\n";
        write(trailer);
        out_.close();
        finished_ = true;
        if (!out_) throw std::runtime_error("sc::pdflite: writing " + path_.string() + " failed");
    }
}
