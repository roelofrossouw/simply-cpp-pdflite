#ifndef SC_PDFLITE_H
#define SC_PDFLITE_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// simply-cpp's own PDF writer: ISO 32000-1 (PDF 1.7), strictly, with no library but zlib. Bare
// bones so far: pages with content streams (paths, colours, clipping, text) and PDF's built-in
// fonts. Three layers: how values are spelt and the file itself (objects, the cross-reference
// table, the trailer); a page's content stream, which refuses what PDF forbids; and the document,
// with its pages, fonts and information. Coordinates are PDF's own: points, y up from the bottom.
namespace sc::pdflite {
    // ---- Values and the file ----

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

    // The file: a header, objects in any order (each written once, by the number allocate() gave
    // it), and on finish() the cross-reference table and trailer. Objects go to the file as they're
    // written; nothing is held but their offsets.
    class file {
    public:
        // Throws std::runtime_error when the file can't be created.
        explicit file(const std::filesystem::path &path);

        // The next object number, to write later (and refer to before then).
        std::uint32_t allocate();

        // Writes object id: "id 0 obj", body, "endobj".
        void object(std::uint32_t id, std::string_view body);

        // Writes object id as a stream: dictionary holds its entries (without /Length or /Filter),
        // and data is compressed with zlib (FlateDecode) when compress is set.
        void stream(std::uint32_t id, std::string_view dictionary, std::string_view data, bool compress = true);

        // Writes the cross-reference table and the trailer, naming root and info (0: none), and
        // closes the file. Throws std::logic_error when an allocated object wasn't written, and
        // std::runtime_error when writing failed.
        void finish(std::uint32_t root, std::uint32_t info);

        [[nodiscard]] bool finished() const { return finished_; }

    private:
        void write(std::string_view bytes);

        std::filesystem::path path_;
        std::ofstream out_;
        std::uint64_t offset_ = 0;
        std::vector<std::uint64_t> offsets_; // by object number - 1; 0 until written
        bool finished_ = false;
    };

    // zlib's deflate of data, as FlateDecode reads it.
    std::string deflate(std::string_view data);

    // ---- A page's content ----

    // A page's content stream, built operator by operator (ISO 32000-1, 8 and 9), in PDF's own
    // coordinates: points, y up from the bottom of the page. It keeps PDF's rules, throwing
    // std::logic_error when an operator would break them: graphics states (q/Q) balance, text only
    // inside BT/ET, a path painted or ended before anything else, Tf before text is shown, and no
    // q/Q, path or nested BT inside a text object. text() holds what's been written so far.
    class content {
    public:
        // Graphics state.
        void save();                  // q
        void restore();               // Q
        void transform(double a, double b, double c, double d, double e, double f); // cm
        void line_width(double width);                                             // w
        void fill_colour(double red, double green, double blue);                   // rg, 0 to 1
        void stroke_colour(double red, double green, double blue);                 // RG, 0 to 1
        void state(std::string_view resource);                                     // gs, a resource name such as "GS1"

        // Paths.
        void move(double x, double y);                                             // m
        void line(double x, double y);                                             // l
        void curve(double x1, double y1, double x2, double y2, double x3, double y3); // c
        void close_path();                                                         // h
        void rectangle(double x, double y, double width, double height);           // re

        // Painting a path (and ending it), or making it the clip.
        void fill(bool even_odd = false);              // f, f*
        void stroke();                                 // S
        void fill_and_stroke(bool even_odd = false);   // B, B*
        void end_path();                               // n
        void clip(bool even_odd = false);              // W, W*; the path is still to be painted or ended

        // Text.
        void begin_text(); // BT
        void end_text();   // ET
        void font(std::string_view resource, double size); // Tf, a resource name such as "F1"
        void text_position(double x, double y);            // Td
        // Tj: bytes in the font's encoding (for the built-in fonts, WinAnsiEncoding).
        void show(std::string_view bytes);

        [[nodiscard]] const std::string &text() const { return text_; }

        // Whether it ends where it may: no unbalanced q, open text object or unfinished path.
        [[nodiscard]] bool complete() const { return saved_.empty() && !in_text_ && !in_path_; }

    private:
        void op(std::string_view line);
        void outside_text(std::string_view what) const;
        void path_step(std::string_view what);
        void paint(std::string_view line);

        std::string text_;
        std::vector<bool> saved_;  // each open q: whether a font was set when it saved the state
        bool in_text_ = false;
        bool font_set_ = false;    // Tf given in the current graphics state
        bool in_path_ = false;
        bool clipping_ = false;  // W given, waiting for the painting operator
    };

    // ---- The document ----

    // A PDF document: pages of one size, each with a content stream, the built-in fonts they use,
    // and the document's information. The file is created at once and finished by close(): the
    // catalog, the page tree, each page with its resources and compressed content, and the
    // information dictionary. It starts with one page, the current one.
    class document {
    public:
        // In points. Throws std::invalid_argument for a size readers can't show (ISO 32000-1 annex
        // C: 3 to 14,400 points), std::runtime_error when the file can't be created.
        document(const std::filesystem::path &path, double width, double height);

        // Closes the file if close() wasn't called, reporting a failure on std::cerr.
        ~document();

        document(const document &) = delete;

        document &operator=(const document &) = delete;

        // Adds a page after the last and makes it current.
        void add_page();

        // Makes page (counting from 1) current; throws std::out_of_range when there's no such page.
        void set_page(std::size_t page);

        [[nodiscard]] std::size_t page() const { return current_ + 1; }

        [[nodiscard]] std::size_t page_count() const { return pages_.size(); }

        [[nodiscard]] double width() const { return width_; }

        [[nodiscard]] double height() const { return height_; }

        // The current page's content.
        content &page_content();

        // The resource name ("F1") of a built-in font in the current page's resources, adding it
        // there (and to the file, once) the first time: one of the twelve Latin standard fonts
        // ("Helvetica", "Helvetica-Bold", "Helvetica-Oblique", "Helvetica-BoldOblique",
        // "Times-Roman", "Times-Bold", "Times-Italic", "Times-BoldItalic", "Courier",
        // "Courier-Bold", "Courier-Oblique", "Courier-BoldOblique"), written in WinAnsiEncoding.
        // Throws std::invalid_argument for another name.
        std::string standard_font(std::string_view base_font);

        // UTF-8 text in WinAnsiEncoding, the built-in fonts' bytes; nullopt when a character isn't
        // in it. Throws std::invalid_argument on malformed UTF-8.
        [[nodiscard]] static std::optional<std::string> win_ansi(std::string_view utf8);

        // An entry of the document information dictionary (14.3.3), UTF-8: "Title", "Author",
        // "Subject", "Keywords", "Creator", "Producer" or a key of one's own. CreationDate and
        // ModDate are set by close(), and Producer when it isn't given; Trapped isn't written.
        // Throws std::invalid_argument for those three keys or an empty one.
        void info(std::string_view key, std::string_view value);

        // Finishes the file. Throws std::logic_error when a page's content isn't complete (an open
        // q, text object or path), std::runtime_error when writing failed; nothing can be added
        // afterwards. Calling it again does nothing.
        void close();

        [[nodiscard]] bool closed() const { return file_.finished(); }

    private:
        struct page_state {
            std::uint32_t id;
            pdflite::content content;
            std::map<std::string, std::string> fonts; // base font: its resource name here
        };

        void check_open() const;

        file file_;
        double width_, height_;
        std::uint32_t pages_id_;
        std::vector<page_state> pages_;
        std::size_t current_ = 0;
        std::map<std::string, std::uint32_t> fonts_; // base font: its object
        std::map<std::string, std::string> info_;    // key: text string, as written
    };
}

#endif //SC_PDFLITE_H
