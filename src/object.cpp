#include "pdflite/object.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
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

    name::name(const std::string_view text) : text_(text) {
        if (text.empty()) throw std::invalid_argument("sc::pdflite: an empty name");
        if (text.find('\0') != std::string_view::npos) throw std::invalid_argument("sc::pdflite: a name with a zero byte");
    }

    std::string name::write() const {
        static constexpr std::string_view delimiters = "()<>[]{}/%#";
        std::string out = "/";
        for (const char c: text_) {
            const auto byte = static_cast<unsigned char>(c);
            if (byte < '!' || byte > '~' || delimiters.find(c) != std::string_view::npos) {
                char escaped[4];
                std::snprintf(escaped, sizeof escaped, "#%02X", byte);
                out += escaped;
            } else out += c;
        }
        return out;
    }

    namespace {
        std::string hex_of(const std::string_view bytes) {
            static constexpr char digits[] = "0123456789ABCDEF";
            std::string out = "<";
            for (const char c: bytes) {
                const auto byte = static_cast<unsigned char>(c);
                out += digits[byte >> 4];
                out += digits[byte & 15];
            }
            return out + '>';
        }
    }

    std::string string::write() const {
        if (hex_) return hex_of(bytes_);
        std::string out = "(";
        for (const char c: bytes_) {
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

    string string::text(const std::string_view utf8) {
        const auto codes = code_points(utf8);
        bool ascii = true;
        for (const char32_t code: codes)
            if (code < ' ' || code > '~') ascii = false;
        if (ascii) return string{std::string{utf8}};
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
        return string{utf16, true};
    }

    string string::date(const std::chrono::system_clock::time_point time) {
        const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
        std::tm local{};
        localtime_r(&seconds, &local);
        char stamp[16];
        std::strftime(stamp, sizeof stamp, "%Y%m%d%H%M%S", &local);
        const long offset = local.tm_gmtoff / 60; // minutes east of UTC
        // Time zones lie within ±14 hours; clamped, so the text is always "+HH'mm'".
        const int minutes = static_cast<int>(std::min(std::labs(offset), 14L * 60));
        char zone[16];
        if (offset == 0) std::snprintf(zone, sizeof zone, "Z");
        else std::snprintf(zone, sizeof zone, "%c%02d'%02d'", offset < 0 ? '-' : '+', minutes / 60, minutes % 60);
        return string{std::string{"D:"} + stamp + zone};
    }

    std::string reference::write() const {
        if (id == 0) throw std::invalid_argument("sc::pdflite: a reference to object 0");
        return std::to_string(id) + " 0 R";
    }

    array::array(const std::initializer_list<object> items) : items_(items) {}

    void array::push_back(object item) { items_.push_back(std::move(item)); }

    std::size_t array::size() const { return items_.size(); }

    bool array::empty() const { return items_.empty(); }

    const object &array::operator[](const std::size_t at) const { return items_.at(at); }

    std::vector<object>::const_iterator array::begin() const { return items_.begin(); }

    std::vector<object>::const_iterator array::end() const { return items_.end(); }

    std::string array::write() const {
        std::string out = "[";
        for (std::size_t i = 0; i < items_.size(); ++i) {
            if (i > 0) out += ' ';
            out += items_[i].write();
        }
        return out + ']';
    }

    dictionary::dictionary(const std::initializer_list<std::pair<std::string_view, object>> entries) {
        for (const auto &[key, value]: entries) add(key, value);
    }

    dictionary &dictionary::add(const std::string_view key, object value) {
        if (contains(key)) throw std::logic_error("sc::pdflite: the key /" + std::string{key} + " twice in one dictionary");
        entries_.emplace_back(name{key}, std::move(value));
        return *this;
    }

    dictionary &dictionary::set(const std::string_view key, object value) {
        for (auto &[existing, old]: entries_)
            if (existing.text() == key) {
                old = std::move(value);
                return *this;
            }
        entries_.emplace_back(name{key}, std::move(value));
        return *this;
    }

    const object *dictionary::find(const std::string_view key) const {
        for (const auto &[existing, value]: entries_)
            if (existing.text() == key) return &value;
        return nullptr;
    }

    const object &dictionary::at(const std::string_view key) const {
        if (const auto *found = find(key)) return *found;
        throw std::out_of_range("sc::pdflite: no /" + std::string{key} + " in the dictionary");
    }

    std::size_t dictionary::size() const { return entries_.size(); }

    bool dictionary::empty() const { return entries_.empty(); }

    std::vector<std::pair<name, object>>::const_iterator dictionary::begin() const { return entries_.begin(); }

    std::vector<std::pair<name, object>>::const_iterator dictionary::end() const { return entries_.end(); }

    std::string dictionary::write() const {
        std::string out = "<<";
        for (const auto &[key, value]: entries_) out += " " + key.write() + " " + value.write();
        return out + " >>";
    }

    object::object(const double value) : value_(value) {
        number(value); // throws when PDF can't have it
    }

    std::int64_t object::checked(const long double integer) {
        if (integer < -2147483648.0L || integer > 2147483647.0L) throw std::invalid_argument("sc::pdflite: an integer beyond 32 bits");
        return static_cast<std::int64_t>(integer);
    }

    std::string object::write() const {
        struct writer {
            std::string operator()(std::nullptr_t) const { return "null"; }
            std::string operator()(const bool value) const { return value ? "true" : "false"; }
            std::string operator()(const std::int64_t value) const { return std::to_string(value); }
            std::string operator()(const double value) const { return number(value); }
            std::string operator()(const name &value) const { return value.write(); }
            std::string operator()(const string &value) const { return value.write(); }
            std::string operator()(const array &value) const { return value.write(); }
            std::string operator()(const dictionary &value) const { return value.write(); }
            std::string operator()(const reference &value) const { return value.write(); }
        };
        return std::visit(writer{}, value_);
    }
}
