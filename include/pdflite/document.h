#ifndef SC_PDFLITE_DOCUMENT_H
#define SC_PDFLITE_DOCUMENT_H

#include "content.h"
#include "file.h"
#include "object.h"

#include <page.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// sc-pdflite's document: pages, the built-in fonts they use, and the document's information.
namespace sc::pdflite {

    // An area of a page, in PDF's coordinates: points, y up from the bottom (a Rect, 7.9.5).
    struct area {
        double left, bottom, right, top;
    };

    // A PDF document: pages of one size, each with a content stream, the built-in fonts they use,
    // and the document's information. The file is created at once and finished by close(): the
    // catalog, the page tree, each page with its resources and compressed content, and the
    // information dictionary. It starts with one page, the current one.
    class document {
    public:
        // In points. Throws std::invalid_argument for a size readers can't show (ISO 32000-1 annex
        // C: 3 to 14,400 points), std::runtime_error when the file can't be created.
        document(const std::filesystem::path &path, double width, double height);

        // A standard paper size (sc-graphics' page.h, shared with sc-pdf and sc-mupdf), portrait
        // or turned to landscape.
        explicit document(const std::filesystem::path &path, sc::page_size paper = sc::page_size::A4,
                          sc::orientation turned = sc::orientation::portrait);

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

        // Makes where (on the current page) a link to url (a URI action, 12.6.4.7). The URI is
        // written in 7-bit ASCII: other bytes, spaces and control characters percent-encoded.
        // Throws std::invalid_argument for an area that's inverted or not finite, or an empty url.
        void link(const area &where, std::string_view url);

        // Makes where (on the current page) a link to page (counting from 1), shown from top (a y
        // in points, from the bottom) down (an /XYZ destination, 12.3.2.2). The page needn't exist
        // yet; close() throws std::out_of_range when it doesn't by then.
        void link(const area &where, std::size_t page, double top);

        // A bookmark in the reader's outline (12.3.3), to page from top down: level 0 is the
        // outermost, and each deeper level goes under the bookmark before it (a level more than
        // one deeper than that one's counts as one deeper). Bookmarks keep the order they're added
        // in and start closed. The page is checked by close(), like a link's.
        void bookmark(std::string_view title, std::size_t page, double top, int level = 0);

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
            struct link {
                array rect;                         // [left bottom right top]
                std::optional<pdflite::string> uri; // a URI action's, or else a page and a top
                std::size_t page;
                double top;
            };
            std::vector<link> links;
        };

        struct outline_item {
            pdflite::string title;
            std::size_t page;
            double top;
            int level;
        };

        // [page /XYZ 0 top null]
        [[nodiscard]] array destination(std::size_t page, double top) const;

        // Writes the outline's objects; returns the root's number (0 without bookmarks).
        std::uint32_t write_outline();

        void check_open() const;

        file file_;
        double width_, height_;
        std::uint32_t pages_id_;
        std::vector<page_state> pages_;
        std::size_t current_ = 0;
        std::map<std::string, std::uint32_t> fonts_; // base font: its object
        std::map<std::string, pdflite::string> info_;
        std::vector<outline_item> outline_;
    };
}

#endif //SC_PDFLITE_DOCUMENT_H
