#ifndef SC_PDFLITE_DOCUMENT_H
#define SC_PDFLITE_DOCUMENT_H

#include "content.h"
#include "file.h"
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// sc-pdflite's document: pages, the built-in fonts they use, and the document's information.
namespace sc::pdflite {

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

#endif //SC_PDFLITE_DOCUMENT_H
