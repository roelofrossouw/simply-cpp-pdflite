#include "pdflite/document.h"

#include "pdflite/object.h"

#include <standard_font.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace sc::pdflite {
    namespace {
        // The standard Type 1 fonts with Latin text (9.6.2.2); Symbol and ZapfDingbats have their
        // own encodings, not WinAnsiEncoding, so they wait until they're needed.
        constexpr std::array<std::string_view, 12> latin_fonts{
            "Helvetica", "Helvetica-Bold", "Helvetica-Oblique", "Helvetica-BoldOblique",
            "Times-Roman", "Times-Bold", "Times-Italic", "Times-BoldItalic",
            "Courier", "Courier-Bold", "Courier-Oblique", "Courier-BoldOblique"};
    }

    namespace {
        // The path, once the page size is one readers can show (checked before the file is made).
        const std::filesystem::path &checked(const std::filesystem::path &path, const double width, const double height) {
            for (const double side: {width, height})
                if (!(side >= 3 && side <= 14400)) throw std::invalid_argument("sc::pdflite: a page side outside 3 to 14,400 points");
            return path;
        }
    }

    document::document(const std::filesystem::path &path, const double width, const double height)
        : file_(checked(path, width, height)), width_(width), height_(height) {
        pages_id_ = file_.allocate();
        add_page();
    }

    namespace {
        std::string rect_of(const area &where) {
            for (const double side: {where.left, where.bottom, where.right, where.top})
                if (!std::isfinite(side)) throw std::invalid_argument("sc::pdflite: an area that isn't finite");
            if (where.right < where.left || where.top < where.bottom) throw std::invalid_argument("sc::pdflite: an inverted area");
            return "[" + number(where.left) + " " + number(where.bottom) + " " + number(where.right) + " " + number(where.top) + "]";
        }

        // A URI in 7-bit ASCII (12.6.4.7): bytes outside ! to ~ percent-encoded, as RFC 3986 has
        // it for UTF-8.
        std::string ascii_uri(const std::string_view url) {
            static constexpr char digits[] = "0123456789ABCDEF";
            std::string out;
            for (const char c: url) {
                const auto byte = static_cast<unsigned char>(c);
                if (byte <= ' ' || byte >= 0x7F) {
                    out += '%';
                    out += digits[byte >> 4];
                    out += digits[byte & 15];
                } else out += c;
            }
            return out;
        }
    }

    document::~document() {
        try {
            close();
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
        }
    }

    void document::check_open() const {
        if (file_.finished()) throw std::logic_error("sc::pdflite: the document is closed");
    }

    void document::add_page() {
        check_open();
        pages_.push_back({file_.allocate(), {}, {}});
        current_ = pages_.size() - 1;
    }

    void document::set_page(const std::size_t page) {
        check_open();
        if (page == 0 || page > pages_.size()) throw std::out_of_range("sc::pdflite: there's no page " + std::to_string(page));
        current_ = page - 1;
    }

    content &document::page_content() {
        check_open();
        return pages_[current_].content;
    }

    std::string document::standard_font(const std::string_view base_font) {
        check_open();
        if (std::ranges::find(latin_fonts, base_font) == latin_fonts.end())
            throw std::invalid_argument("sc::pdflite: " + std::string{base_font} + " isn't one of the built-in fonts");
        auto &page = pages_[current_];
        const std::string key{base_font};
        if (const auto found = page.fonts.find(key); found != page.fonts.end()) return found->second;
        if (!fonts_.contains(key)) fonts_[key] = file_.allocate();
        const std::string resource = "F" + std::to_string(page.fonts.size() + 1);
        page.fonts[key] = resource;
        return resource;
    }

    std::optional<std::string> document::win_ansi(const std::string_view utf8) {
        std::string out;
        for (const char32_t code: code_points(utf8)) {
            const auto byte = sc::standard_font::encode(code);
            if (!byte) return std::nullopt;
            out += *byte;
        }
        return out;
    }

    void document::info(const std::string_view key, const std::string_view value) {
        check_open();
        if (key.empty() || key == "CreationDate" || key == "ModDate" || key == "Trapped")
            throw std::invalid_argument("sc::pdflite: the information key " + std::string{key} + " can't be set");
        info_[std::string{key}] = text_string(value);
    }

    void document::link(const area &where, const std::string_view url) {
        check_open();
        if (url.empty()) throw std::invalid_argument("sc::pdflite: a link to an empty URI");
        pages_[current_].links.push_back({rect_of(where), literal(ascii_uri(url)), 0, 0});
    }

    void document::link(const area &where, const std::size_t page, const double top) {
        check_open();
        if (page == 0) throw std::out_of_range("sc::pdflite: pages count from 1");
        number(top); // throws when it isn't finite
        pages_[current_].links.push_back({rect_of(where), "", page, top});
    }

    void document::bookmark(const std::string_view title, const std::size_t page, const double top, const int level) {
        check_open();
        if (page == 0) throw std::out_of_range("sc::pdflite: pages count from 1");
        number(top);
        outline_.push_back({text_string(title), page, top, std::max(0, level)});
    }

    std::string document::destination(const std::size_t page, const double top) const {
        return "[" + reference(pages_[page - 1].id) + " /XYZ 0 " + number(top) + " null]";
    }

    std::uint32_t document::write_outline() {
        if (outline_.empty()) return 0;
        const std::size_t count = outline_.size();
        std::vector<std::uint32_t> ids(count);
        for (auto &id: ids) id = file_.allocate();
        const std::uint32_t root = file_.allocate();
        // Each item's parent: the nearest item before it at a shallower level, or the root.
        std::vector<std::optional<std::size_t>> parent(count);
        std::vector<std::size_t> open; // items still taking children, outermost first
        for (std::size_t i = 0; i < count; ++i) {
            while (!open.empty() && outline_[open.back()].level >= outline_[i].level) open.pop_back();
            if (!open.empty()) parent[i] = open.back();
            open.push_back(i);
        }
        std::map<std::optional<std::size_t>, std::vector<std::size_t>> children; // in order
        for (std::size_t i = 0; i < count; ++i) children[parent[i]].push_back(i);
        for (std::size_t i = 0; i < count; ++i) {
            const auto &siblings = children[parent[i]];
            const auto at = static_cast<std::size_t>(std::ranges::find(siblings, i) - siblings.begin());
            std::string entry = "<< /Title " + outline_[i].title + " /Parent " + reference(parent[i] ? ids[*parent[i]] : root);
            if (at > 0) entry += " /Prev " + reference(ids[siblings[at - 1]]);
            if (at + 1 < siblings.size()) entry += " /Next " + reference(ids[siblings[at + 1]]);
            if (const auto found = children.find(i); found != children.end()) {
                // Closed: the count is negative, the number of children that opening it shows (12.3.3).
                entry += " /First " + reference(ids[found->second.front()]) + " /Last " + reference(ids[found->second.back()]) +
                         " /Count -" + std::to_string(found->second.size());
            }
            entry += " /Dest " + destination(outline_[i].page, outline_[i].top) + " >>";
            file_.object(ids[i], entry);
        }
        const auto &top_level = children[std::nullopt];
        file_.object(root, "<< /Type /Outlines /First " + reference(ids[top_level.front()]) + " /Last " + reference(ids[top_level.back()]) +
                           " /Count " + std::to_string(top_level.size()) + " >>");
        return root;
    }

    void document::close() {
        if (file_.finished()) return;
        // Everything is checked before anything is written, so a mistake leaves nothing half-done.
        for (std::size_t i = 0; i < pages_.size(); ++i)
            if (!pages_[i].content.complete())
                throw std::logic_error("sc::pdflite: page " + std::to_string(i + 1) + "'s content isn't complete (an open q, text object or path)");
        for (const auto &page: pages_)
            for (const auto &each: page.links)
                if (each.uri.empty() && each.page > pages_.size())
                    throw std::out_of_range("sc::pdflite: a link to page " + std::to_string(each.page) + ", which isn't there");
        for (const auto &item: outline_)
            if (item.page > pages_.size()) throw std::out_of_range("sc::pdflite: a bookmark to page " + std::to_string(item.page) + ", which isn't there");

        const std::string media_box = "[0 0 " + number(width_) + " " + number(height_) + "]";
        std::string kids;
        for (auto &page: pages_) {
            const std::uint32_t contents = file_.allocate();
            file_.stream(contents, "", page.content.text());
            std::string resources = "<<";
            if (!page.fonts.empty()) {
                resources += " /Font <<";
                for (const auto &[base_font, resource]: page.fonts) resources += " " + name(resource) + " " + reference(fonts_.at(base_font));
                resources += " >>";
            }
            resources += " >>";
            // Link annotations (12.5.6.5): printed (/F 4), no border.
            std::string annotations;
            for (const auto &each: page.links) {
                const std::uint32_t id = file_.allocate();
                const std::string target = each.uri.empty() ? " /Dest " + destination(each.page, each.top)
                                                            : " /A << /Type /Action /S /URI /URI " + each.uri + " >>";
                file_.object(id, "<< /Type /Annot /Subtype /Link /Rect " + each.rect + " /F 4 /Border [0 0 0]" + target + " >>");
                annotations += (annotations.empty() ? "" : " ") + reference(id);
            }
            file_.object(page.id, "<< /Type /Page /Parent " + reference(pages_id_) + " /MediaBox " + media_box +
                                  " /Resources " + resources + " /Contents " + reference(contents) +
                                  (annotations.empty() ? "" : " /Annots [" + annotations + "]") + " >>");
            kids += (kids.empty() ? "" : " ") + reference(page.id);
            page.content = {}; // written: its text isn't needed any more
        }
        file_.object(pages_id_, "<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pages_.size()) + " >>");

        for (const auto &[base_font, id]: fonts_)
            file_.object(id, "<< /Type /Font /Subtype /Type1 /BaseFont " + name(base_font) + " /Encoding /WinAnsiEncoding >>");

        const std::uint32_t outlines = write_outline();
        const std::uint32_t catalog = file_.allocate();
        file_.object(catalog, "<< /Type /Catalog /Pages " + reference(pages_id_) +
                              (outlines ? " /Outlines " + reference(outlines) + " /PageMode /UseOutlines" : std::string{}) + " >>");

        const auto now = date(std::chrono::system_clock::now());
        if (!info_.contains("Producer")) info_["Producer"] = text_string("simply-cpp-pdf");
        std::string entries;
        for (const auto &[key, value]: info_) entries += " " + name(key) + " " + value;
        const std::uint32_t information = file_.allocate();
        file_.object(information, "<<" + entries + " /CreationDate " + now + " /ModDate " + now + " >>");

        file_.finish(catalog, information);
    }
}
