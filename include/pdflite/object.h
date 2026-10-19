#ifndef SC_PDFLITE_OBJECT_H
#define SC_PDFLITE_OBJECT_H

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// sc-pdflite's objects (ISO 32000-1, 7.3): PDF's values, each of which writes itself as PDF spells
// it. object holds any one of them. They're checked as they're made, so whatever is written is
// valid: a number is finite and within what readers must handle, a name isn't empty, a dictionary
// has each key once.
namespace sc::pdflite {
    // A number as PDF spells it (7.3.3): an integer when whole, else at most four decimals, never
    // an exponent (PDF has none), "0" rather than "-0". Throws std::invalid_argument when it isn't
    // finite or lies outside what readers must handle (annex C: about ±3.4e38).
    std::string number(double value);

    // The code points of UTF-8 text; throws std::invalid_argument when it's malformed.
    std::u32string code_points(std::string_view utf8);

    // A name (7.3.5): "/Helvetica"; bytes outside ! to ~ and the delimiters written #xx. Throws
    // std::invalid_argument when it's empty or holds a zero byte.
    class name {
    public:
        explicit name(std::string_view text);

        [[nodiscard]] const std::string &text() const { return text_; }

        [[nodiscard]] std::string write() const;

        friend bool operator==(const name &, const name &) = default;

    private:
        std::string text_;
    };

    // A string (7.3.4): bytes, written as a literal "(...)" (( ) and \ escaped, other bytes outside
    // space to ~ as octal) or in hex "<...>".
    class string {
    public:
        explicit string(std::string bytes, bool hex = false) : bytes_(std::move(bytes)), hex_(hex) {}

        // A text string (7.9.2.2) from UTF-8: a literal when it's printable ASCII, else UTF-16BE with
        // its byte order mark, in hex. Throws std::invalid_argument on malformed UTF-8.
        [[nodiscard]] static string text(std::string_view utf8);

        // A date (7.9.4): (D:20261019143812+02'00'), in the computer's time zone.
        [[nodiscard]] static string date(std::chrono::system_clock::time_point time);

        [[nodiscard]] const std::string &bytes() const { return bytes_; }

        [[nodiscard]] bool hex() const { return hex_; }

        [[nodiscard]] std::string write() const;

        friend bool operator==(const string &, const string &) = default;

    private:
        std::string bytes_;
        bool hex_;
    };

    // An indirect object's reference (7.3.10): "12 0 R". Numbers start at 1.
    struct reference {
        std::uint32_t id;

        [[nodiscard]] std::string write() const;

        friend bool operator==(const reference &, const reference &) = default;
    };

    class object;

    // An array (7.3.6): "[a b c]".
    class array {
    public:
        array() = default;

        array(std::initializer_list<object> items);

        void push_back(object item);

        [[nodiscard]] std::size_t size() const;

        [[nodiscard]] bool empty() const;

        [[nodiscard]] const object &operator[](std::size_t at) const;

        [[nodiscard]] std::vector<object>::const_iterator begin() const;

        [[nodiscard]] std::vector<object>::const_iterator end() const;

        [[nodiscard]] std::string write() const;

    private:
        std::vector<object> items_;
    };

    // A dictionary (7.3.7): "<< /Key value ... >>", its entries in the order they're added. Each key
    // is a name, there once: add() throws std::logic_error for a key it already has.
    class dictionary {
    public:
        dictionary() = default;

        dictionary(std::initializer_list<std::pair<std::string_view, object>> entries);

        // Adds key; throws std::logic_error when it's there already.
        dictionary &add(std::string_view key, object value);

        // Adds key, or replaces its value.
        dictionary &set(std::string_view key, object value);

        [[nodiscard]] bool contains(std::string_view key) const { return find(key) != nullptr; }

        // key's value, or nullptr.
        [[nodiscard]] const object *find(std::string_view key) const;

        // key's value; throws std::out_of_range when it isn't there.
        [[nodiscard]] const object &at(std::string_view key) const;

        [[nodiscard]] std::size_t size() const;

        [[nodiscard]] bool empty() const;

        [[nodiscard]] std::vector<std::pair<name, object>>::const_iterator begin() const;

        [[nodiscard]] std::vector<std::pair<name, object>>::const_iterator end() const;

        [[nodiscard]] std::string write() const;

    private:
        std::vector<std::pair<name, object>> entries_;
    };

    // Any of PDF's direct values: null, a boolean, an integer, a real, a name, a string, an array,
    // a dictionary or a reference (streams are only indirect: see file::stream()). Integers are
    // kept within 32 bits (annex C), so a larger one throws std::invalid_argument; a real is
    // checked as number() checks it.
    class object {
    public:
        using value_type = std::variant<std::nullptr_t, bool, std::int64_t, double, name, string, array, dictionary, reference>;

        object() : value_(nullptr) {}

        object(std::nullptr_t) : value_(nullptr) {}

        object(bool value) : value_(value) {}

        template<std::integral Integer> requires (!std::same_as<Integer, bool>)
        object(const Integer value) : value_(checked(static_cast<long double>(value))) {}

        object(double value);

        object(name value) : value_(std::move(value)) {}

        object(string value) : value_(std::move(value)) {}

        object(array value) : value_(std::move(value)) {}

        object(dictionary value) : value_(std::move(value)) {}

        object(reference value) : value_(value) {}

        // Text isn't an object until it's a name or a string: say which.
        object(const char *) = delete;

        object(std::string_view) = delete;

        object(const std::string &) = delete;

        template<typename T>
        [[nodiscard]] bool is() const { return std::holds_alternative<T>(value_); }

        // The value as T; throws std::bad_variant_access when it's another kind.
        template<typename T>
        [[nodiscard]] const T &as() const { return std::get<T>(value_); }

        [[nodiscard]] const value_type &value() const { return value_; }

        [[nodiscard]] std::string write() const;

    private:
        static std::int64_t checked(long double integer);

        value_type value_;
    };
}

#endif //SC_PDFLITE_OBJECT_H
