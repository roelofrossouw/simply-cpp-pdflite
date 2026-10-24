#ifndef SC_PDFLITE_DOCUMENT_H
#define SC_PDFLITE_DOCUMENT_H

#include "content.h"
#include "encryption.h"
#include "file.h"
#include "font.h"
#include "image.h"

#include <memory>
#include "object.h"

#include <distance.h>
#include <page.h>
#include <rect.h>
#include <size.h>
#include <standard_font.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sc::pdflite {
    // A PDF document: pages, each of its own size (the document's unless add_page() gives one),
    // each with a content stream and the fonts, pictures and graphics states it uses; links,
    // bookmarks and the document's information. The file is created at once and finished by
    // close(): the catalog, the page tree, each page with its resources and compressed content,
    // the outline and the information dictionary. It starts with one page, the current one.
    class document {
    public:
        // A size of distances ({sc::dist::mm(210), sc::dist::mm(297)}, or points). Throws std::invalid_argument for a size
        // readers can't show (ISO 32000-1 annex C: 3 to 14,400 points), std::runtime_error when the
        // file can't be created.
        document(const std::filesystem::path &path, const sc::size_<sc::distance> &size);

        // A standard paper size (sc-graphics' page.h, shared with sc-pdf and sc-mupdf), portrait
        // or turned to landscape.
        explicit document(const std::filesystem::path &path, sc::page_size paper = sc::page_size::A4,
                          sc::orientation turned = sc::orientation::portrait);

        // Closes the file if close() wasn't called, reporting a failure on std::cerr.
        ~document();

        document(const document &) = delete;

        document &operator=(const document &) = delete;

        // Adds a page after the last and makes it current: of the document's size (the one it was
        // made with), or of its own. Pages of different sizes can be mixed in one file. Throws
        // std::invalid_argument for a size readers can't show, as the constructor does.
        void add_page();

        void add_page(const sc::size_<sc::distance> &size);

        void add_page(sc::page_size paper, sc::orientation turned = sc::orientation::portrait);

        // Makes page (counting from 1) current; throws std::out_of_range when there's no such page.
        void set_page(std::size_t page);

        [[nodiscard]] std::size_t page() const { return current_ + 1; }

        [[nodiscard]] std::size_t page_count() const { return pages_.size(); }

        // The current page's size: << writes [595.276pt,841.89pt], and
        // static_cast<sc::size_<sc::dist::mm>>(size()) is [210mm,297mm].
        [[nodiscard]] sc::size_<sc::distance> size() const { return {pages_[current_].width, pages_[current_].height}; }

        content &page_content();

        // The resource name ("F1") of a built-in font in the current page's resources, adding it
        // there (and to the file, once) the first time: one of sc-graphics' twelve Latin standard
        // fonts (sc::standard_font::face::helvetica_bold, ...), written in WinAnsiEncoding. Their
        // metrics are sc::standard_font's: sc::standard_font{face}.width(text, size).
        std::string standard_font(sc::standard_font::face which);

        // The resource name ("F2") of an embedded font in the current page's resources, adding it
        // there (and to the file, once) the first time; put its text into glyphs with
        // font.encode() for content::show(). Written by close(), as a subset of every glyph used.
        std::string font(const pdflite::font &embedded);

        // The resource name ("Im1") of picture (a jpeg, png or svg) in the current page's
        // resources, adding it there (and to the file, once) the first time; draw it with
        // content::draw(). The same picture used again, on any page, is written once: a copy of
        // it, or another read from the same bytes.
        std::string image(const pdflite::image &picture);

        // The same, for picture drawn shown big (on the page, its unit square's sides), which
        // image_resolution() goes by.
        std::string image(const pdflite::image &picture, const sc::size_<sc::distance> &shown);

        // Writes PNGs at most about pixels_per_inch where they're shown: one with more than 1.5
        // times that at its biggest showing (image() with its size) is scaled down to it
        // (png::reduced()), once, for every page. One also drawn without its size, a JPEG and an
        // SVG are written as they are. 0, the default, keeps every picture as it is. Throws
        // std::invalid_argument for a negative or non-finite resolution.
        void image_resolution(double pixels_per_inch);

        // The resource name ("GS1") of a graphics state (8.4.5, an ExtGState) that paints fills and
        // text at fill_alpha and strokes at stroke_alpha (0 transparent to 1 opaque), in the current
        // page's resources, adding it there (and to the file, once) the first time; set it with
        // content::state(), inside q ... Q so it doesn't stay. Throws std::invalid_argument for an
        // alpha outside 0 to 1.
        std::string graphics_state(double fill_alpha, double stroke_alpha = 1);

        // UTF-8 text in WinAnsiEncoding, the built-in fonts' bytes; nullopt when a character isn't
        // in it. Throws std::invalid_argument on malformed UTF-8.
        [[nodiscard]] static std::optional<std::string> win_ansi(std::string_view utf8);

        // An entry of the document information dictionary (14.3.3), UTF-8: "Title", "Author",
        // "Subject", "Keywords", "Creator", "Producer" or a key of one's own. CreationDate and
        // ModDate are set by close(), and Producer when it isn't given; Trapped isn't written.
        // Throws std::invalid_argument for those three keys or an empty one.
        void info(std::string_view key, std::string_view value);

        // Makes where (on the current page; points from its top-left corner, y down, as on
        // sc-graphics' canvas) a link to url (a URI action, 12.6.4.7). The URI is written in 7-bit
        // ASCII: other bytes, spaces and control characters percent-encoded. Throws
        // std::invalid_argument for an area that's negative or not finite, or an empty url.
        void link(const sc::rect &where, std::string_view url);

        // Makes where a link to page (counting from 1), shown from top (points from the page's top)
        // down (an /XYZ destination, 12.3.2.2). The page needn't exist yet; close() throws
        // std::out_of_range when it doesn't by then.
        void link(const sc::rect &where, std::size_t page, sc::distance top = 0);

        // A bookmark in the reader's outline (12.3.3), to page from top down: level 0 is the
        // outermost, and each deeper level goes under the bookmark before it (a level more than
        // one deeper than that one's counts as one deeper). Bookmarks keep the order they're added
        // in and start closed. The page is checked by close(), like a link's.
        void bookmark(std::string_view title, std::size_t page, sc::distance top = 0, int level = 0);

        // Encrypts the file (AES-256, pdflite/encryption.h) with these passwords and permissions, as
        // close() writes it; any time before close(). Throws std::invalid_argument without a
        // password.
        void protect(const protection &settings);

        // Finishes the file. Throws std::logic_error when a page's content isn't complete (an open
        // q, text object or path), std::runtime_error when writing failed; nothing can be added
        // afterwards. Calling it again does nothing.
        void close();

        [[nodiscard]] bool closed() const { return file_.finished(); }

    private:
        struct page_state {
            std::uint32_t id;
            double width, height;
            pdflite::content content;
            std::map<sc::standard_font::face, std::string> fonts; // each font: its resource name here
            std::map<const void *, std::string> images;           // each picture (by its identity): its name here
            std::map<const void *, std::string> embedded;         // each embedded font (by its identity): its name here
            std::map<std::pair<double, double>, std::string> states; // each graphics state (fill, stroke alpha): its name here
            struct link {
                array rect;                         // [left bottom right top], in PDF's coordinates
                std::optional<pdflite::string> uri; // a URI action's, or else a page and a top
                std::size_t page;
                double top;                         // from the page's top
            };
            std::vector<link> links;
        };

        struct outline_item {
            pdflite::string title;
            std::size_t page;
            double top;
            int level;
        };

        // where as PDF's Rect, [left bottom right top] with y up.
        [[nodiscard]] array rect_of(const sc::rect &where) const;

        // [page /XYZ 0 top null], top turned into PDF's y.
        [[nodiscard]] array destination(std::size_t page, double top) const;

        // Writes the outline's objects; returns the root's number (0 without bookmarks).
        std::uint32_t write_outline();

        void check_open() const;

        file file_;
        double width_, height_; // the document's: for add_page() without a size
        std::uint32_t pages_id_;
        std::vector<page_state> pages_;
        std::size_t current_ = 0;
        std::map<sc::standard_font::face, std::uint32_t> fonts_; // each font: its object
        struct written_image {
            std::shared_ptr<const pdflite::image> picture; // held, so its identity stays unique
            std::uint32_t id;
            double widest = 0, tallest = 0; // the biggest it's shown, in points
            bool unsized = false;           // drawn somewhere without its size
        };
        std::map<const void *, written_image> images_; // by the picture's identity, which copies share
        std::map<std::string_view, const void *> encoded_images_; // a picture's bytes (in images_): its identity there
        double image_resolution_ = 0; // pixels per inch; 0 keeps pictures as they are

        // image()'s work: shown, when it's known, counts towards its resolution.
        std::string placed(const pdflite::image &picture, const std::optional<sc::size_<sc::distance>> &shown);

        // The picture as close() writes it: scaled down to the resolution when it's that much bigger.
        [[nodiscard]] std::shared_ptr<const pdflite::image> as_written(const written_image &written) const;
        struct written_font {
            pdflite::font embedded; // held: copies share its glyphs
            std::uint32_t id;
        };
        std::map<const void *, written_font> embedded_; // by the font's identity
        std::map<std::pair<double, double>, std::uint32_t> states_; // each graphics state: its object
        std::map<std::string, pdflite::string> info_;
        std::vector<outline_item> outline_;
        std::shared_ptr<const encryption> encryption_;
    };
}

#endif //SC_PDFLITE_DOCUMENT_H
