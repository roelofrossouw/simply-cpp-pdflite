#pragma once

#include <sc/pdflite/encryption.h>

#include <sc/graphics/canvas.h>
#include <sc/graphics/font.h>
#include <sc/graphics/page.h>
#include <sc/core/rect.h>
#include <sc/graphics/text.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace sc::pdflite {
    // A PDF file drawn as an sc::canvas: the same drawing and measuring calls as an sc::image (or
    // sc::mupdf::writer, MuPDF's), plus images, links and bookmarks, written by sc::pdflite::document.
    // Positions are in points (1/72 inch) from the page's top-left corner, y down. A colour's alpha
    // is honoured; fonts are found through fontconfig and embedded as subsets of the glyphs used.
    // Drawing goes to the current page: the newest, unless set_page() moved back. close() finishes
    // the file, or else the destructor, which can't report a failure.
    class writer : public sc::canvas {
    public:
        // Starts the file with one page; every page has this size and orientation (page.h). Throws
        // std::runtime_error when the file can't be created.
        explicit writer(const std::filesystem::path &filename, page_size size = page_size::A4, orientation turned = orientation::portrait);

        // The same, encrypted (AES-256, as PDF 2.0 and Adobe's extension level 8 have it) with these
        // passwords and permissions (encryption.h). With neither password the file isn't encrypted.
        writer(const std::filesystem::path &filename, page_size size, orientation turned, const protection &locked);

        ~writer() override;

        writer(const writer &) = delete;

        writer &operator=(const writer &) = delete;

        writer(writer &&) noexcept;

        writer &operator=(writer &&) noexcept;

        // Writes out the pages and finishes the file; throws std::runtime_error when that fails.
        // Nothing can be drawn afterwards. Calling it again does nothing.
        void close();

        // Adds a page after the last one and makes it current.
        void new_page();

        // Makes page (counting from 1) current, adding pages up to it when there are fewer.
        void set_page(unsigned page);

        [[nodiscard]] unsigned page() const;

        [[nodiscard]] unsigned page_count() const;

        // In points, landscape already turned.
        [[nodiscard]] double page_width() const;

        [[nodiscard]] double page_height() const;

        // A JPEG or PNG image, scaled to fit pos with its aspect ratio kept and centred in it.
        // Throws std::runtime_error when the file can't be read as an image.
        void draw_image(const std::filesystem::path &filename, const rect &pos);

        // Makes pos on the current page a link to url.
        void add_link(const std::string &url, const rect &pos);

        // Makes pos on the current page a link to page (counting from 1), shown from top (points
        // from its top) down. The page needn't exist yet, but must by close().
        void add_link(unsigned page, const rect &pos, double top = 0);

        // A bookmark in the reader's outline, to page from top down; level 0 is the outermost, each
        // deeper level under the bookmark before it. Bookmarks keep the order they're added in.
        void add_outline(const std::string &title, unsigned page, double top = 0, int level = 0);

        // Text in Helvetica, Times or Courier (any case; Arial, Times New Roman and Courier New
        // too), bold and italic as asked, is written in PDF's built-in fonts, which every reader has:
        // nothing embedded, smaller files. It's placed and measured with Adobe's metrics for them
        // (sc::standard_font), so it fits as readers draw it. Characters outside Windows-1252, which
        // the built-in fonts are written in, are drawn in the installed font, embedded. Off by default.
        void builtin_fonts(bool on);

        [[nodiscard]] bool builtin_fonts() const;

        // Writes PNG pictures at most about pixels_per_inch at the biggest size they're drawn: one
        // with more than 1.5 times that is scaled down (sc::pdflite::document::image_resolution()).
        // JPEGs are written as they are. 0, the default, keeps every picture as it is. Throws
        // std::invalid_argument for a negative or non-finite resolution.
        void image_resolution(double pixels_per_inch);

        // Sets the document's information: "Title", "Author", "Subject", "Keywords", "Creator",
        // "Producer" or a key of your own (UTF-8), as sc::mupdf::document::metadata() reads it.
        void metadata(const std::string &key, const std::string &value);

    protected:
        // Strokes are a colour and a width, fills a colour and a fill rule (even-odd or nonzero);
        // alpha below 1 goes through a graphics state (ExtGState). Other stroke and fill styles
        // aren't drawn yet. Text throws std::runtime_error when no font is found.
        void paint_polygon(const polygon &shape, const sc::appearance &look) override;

        void paint_circle(const circle &shape, const sc::appearance &look) override;

        void paint_line(const line &shape, const sc::stroke &stroke) override;

        void paint_text(const text &sample, const sc::fill &fill) override;

        // Each bitmap is written into the file once (one of the same bytes as another counts as
        // it), a JPEG as it is, and used from every page.
        // Throws std::runtime_error when it can't be read.
        void paint_bitmap(const bitmap &picture, const transform &placement) override;

        // In points, in the font fontconfig finds (std::runtime_error when there's none). x and cap
        // heights are what the font declares, or else the letter's own height.
        double measure_width(const text &sample) override;

        double measure_x_height(const sc::font &font) override;

        double measure_cap_height(const sc::font &font) override;

        // Real quarter circles, not the polygon a canvas draws by default.
        void paint_rounded_rect(const rounded_rect &shape, const sc::appearance &look) override;

        // Real curves.
        void paint_path(const path &shape, const sc::appearance &look) override;

    public:
        // Clips are nonzero and nest.
        void push_clip(const path &shape) override;

        void pop_clip() override;

    private:
        struct impl;
        std::unique_ptr<impl> impl_;
    };
}

