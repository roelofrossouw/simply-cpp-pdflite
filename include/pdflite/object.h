#ifndef SC_PDFLITE_OBJECT_H
#define SC_PDFLITE_OBJECT_H

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

// sc-pdflite's objects (ISO 32000-1, 7.3): how each kind of value is written.
namespace sc::pdflite {

    // A number as PDF reads it: an integer when whole, else at most four decimals, never an
    // exponent (PDF has none), "0" rather than "-0". Throws std::invalid_argument when it isn't
    // finite or lies outside what readers must handle (ISO 32000-1 annex C: about ±3.4e38, and
    // integers within 32 bits).
    std::string number(double value);

    // A name: "/Helvetica"; bytes outside ! to ~ and the delimiters are written #xx (7.3.5).
    // Throws std::invalid_argument when it's empty or holds a zero byte.
    std::string name(std::string_view text);

    // A literal string, "(...)": ( ) and \ escaped, other bytes outside space to ~ as octal (7.3.4.2).
    std::string literal(std::string_view bytes);

    // A hex string, "<...>".
    std::string hex(std::string_view bytes);

    // The code points of UTF-8 text; throws std::invalid_argument when it's malformed.
    std::u32string code_points(std::string_view utf8);

    // A text string (7.9.2.2), from UTF-8: a literal when it's printable ASCII, else UTF-16BE with
    // its byte order mark, in hex. Throws std::invalid_argument on malformed UTF-8.
    std::string text_string(std::string_view utf8);

    // A date (7.9.4): (D:20261019143812+02'00'), in the computer's time zone.
    std::string date(std::chrono::system_clock::time_point time);

    // An indirect reference: "12 0 R".
    std::string reference(std::uint32_t id);
}

#endif //SC_PDFLITE_OBJECT_H
