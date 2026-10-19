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
        array rect_of(const area &where) {
            array rect{where.left, where.bottom, where.right, where.top}; // throws when one isn't finite
            if (where.right < where.left || where.top < where.bottom) throw std::invalid_argument("sc::pdflite: an inverted area");
            return rect;
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

    document::document(const std::filesystem::path &path, const sc::page_size paper, const sc::orientation turned)
        : document(path, sc::page_dimensions(paper, turned).width(), sc::page_dimensions(paper, turned).height()) {}

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
        info_.insert_or_assign(std::string{key}, string::text(value));
    }

    void document::link(const area &where, const std::string_view url) {
        check_open();
        if (url.empty()) throw std::invalid_argument("sc::pdflite: a link to an empty URI");
        pages_[current_].links.push_back({rect_of(where), string{ascii_uri(url)}, 0, 0});
    }

    void document::link(const area &where, const std::size_t page, const double top) {
        check_open();
        if (page == 0) throw std::out_of_range("sc::pdflite: pages count from 1");
        number(top); // throws when it isn't finite
        pages_[current_].links.push_back({rect_of(where), std::nullopt, page, top});
    }

    void document::bookmark(const std::string_view title, const std::size_t page, const double top, const int level) {
        check_open();
        if (page == 0) throw std::out_of_range("sc::pdflite: pages count from 1");
        number(top);
        outline_.push_back({string::text(title), page, top, std::max(0, level)});
    }

    array document::destination(const std::size_t page, const double top) const {
        return {reference{pages_[page - 1].id}, name{"XYZ"}, 0, top, nullptr};
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
            dictionary entry{{"Title", outline_[i].title}, {"Parent", reference{parent[i] ? ids[*parent[i]] : root}}};
            if (at > 0) entry.add("Prev", reference{ids[siblings[at - 1]]});
            if (at + 1 < siblings.size()) entry.add("Next", reference{ids[siblings[at + 1]]});
            if (const auto found = children.find(i); found != children.end()) {
                entry.add("First", reference{ids[found->second.front()]});
                entry.add("Last", reference{ids[found->second.back()]});
                // Closed: the count is negative, the number of children that opening it shows (12.3.3).
                entry.add("Count", -static_cast<long long>(found->second.size()));
            }
            entry.add("Dest", destination(outline_[i].page, outline_[i].top));
            file_.write(ids[i], entry);
        }
        const auto &top_level = children[std::nullopt];
        file_.write(root, dictionary{{"Type", name{"Outlines"}}, {"First", reference{ids[top_level.front()]}},
                                     {"Last", reference{ids[top_level.back()]}}, {"Count", top_level.size()}});
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
                if (!each.uri && each.page > pages_.size())
                    throw std::out_of_range("sc::pdflite: a link to page " + std::to_string(each.page) + ", which isn't there");
        for (const auto &item: outline_)
            if (item.page > pages_.size()) throw std::out_of_range("sc::pdflite: a bookmark to page " + std::to_string(item.page) + ", which isn't there");

        const array media_box{0, 0, width_, height_};
        array kids;
        for (auto &page: pages_) {
            const std::uint32_t contents = file_.allocate();
            file_.write_stream(contents, {}, page.content.text());
            dictionary resources;
            if (!page.fonts.empty()) {
                dictionary fonts;
                for (const auto &[base_font, resource]: page.fonts) fonts.add(resource, reference{fonts_.at(base_font)});
                resources.add("Font", fonts);
            }
            // Link annotations (12.5.6.5): printed (/F 4), no border.
            array annotations;
            for (const auto &each: page.links) {
                const std::uint32_t id = file_.allocate();
                dictionary annotation{{"Type", name{"Annot"}}, {"Subtype", name{"Link"}}, {"Rect", each.rect}, {"F", 4},
                                      {"Border", array{0, 0, 0}}};
                if (each.uri) annotation.add("A", dictionary{{"Type", name{"Action"}}, {"S", name{"URI"}}, {"URI", *each.uri}});
                else annotation.add("Dest", destination(each.page, each.top));
                file_.write(id, annotation);
                annotations.push_back(reference{id});
            }
            dictionary entry{{"Type", name{"Page"}}, {"Parent", reference{pages_id_}}, {"MediaBox", media_box},
                             {"Resources", resources}, {"Contents", reference{contents}}};
            if (!annotations.empty()) entry.add("Annots", annotations);
            file_.write(page.id, entry);
            kids.push_back(reference{page.id});
            page.content = {}; // written: its text isn't needed any more
        }
        file_.write(pages_id_, dictionary{{"Type", name{"Pages"}}, {"Kids", kids}, {"Count", pages_.size()}});

        for (const auto &[base_font, id]: fonts_)
            file_.write(id, dictionary{{"Type", name{"Font"}}, {"Subtype", name{"Type1"}}, {"BaseFont", name{base_font}},
                                       {"Encoding", name{"WinAnsiEncoding"}}});

        const std::uint32_t outlines = write_outline();
        const std::uint32_t catalog = file_.allocate();
        dictionary root{{"Type", name{"Catalog"}}, {"Pages", reference{pages_id_}}};
        if (outlines) root.add("Outlines", reference{outlines}).add("PageMode", name{"UseOutlines"});
        file_.write(catalog, root);

        const auto now = string::date(std::chrono::system_clock::now());
        if (!info_.contains("Producer")) info_.insert_or_assign("Producer", string::text("simply-cpp-pdflite"));
        dictionary information;
        for (const auto &[key, value]: info_) information.add(key, value);
        information.add("CreationDate", now).add("ModDate", now);
        const std::uint32_t information_id = file_.allocate();
        file_.write(information_id, information);

        file_.finish(catalog, information_id);
    }
}
