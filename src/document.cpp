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
        // Throws std::invalid_argument for a page size readers can't show (annex C).
        void check_size(const sc::distance width, const sc::distance height) {
            for (const double side: {width.points(), height.points()})
                if (!(side >= 3 && side <= 14400)) throw std::invalid_argument("sc::pdflite: a page side outside 3 to 14,400 points");
        }

        // The path, once the page size is one readers can show (checked before the file is made).
        const std::filesystem::path &checked(const std::filesystem::path &path, const sc::distance width, const sc::distance height) {
            check_size(width, height);
            return path;
        }

        // The page's name for key, or the first time, after write_once() has made sure the file
        // gets it, prefix and number.
        template <typename Key, typename WriteOnce>
        std::string page_name(std::map<Key, std::string> &names, const Key &key, WriteOnce &&write_once, const std::string_view prefix,
                              const std::size_t number) {
            if (const auto found = names.find(key); found != names.end()) return found->second;
            write_once();
            return names[key] = std::string{prefix} + std::to_string(number);
        }
    }

    document::document(const std::filesystem::path &path, const sc::size_<sc::distance> &size)
        : file_(checked(path, size.width(), size.height())), width_(size.width()), height_(size.height()) {
        pages_id_ = file_.allocate();
        add_page();
    }

    namespace {
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
        : document(path, sc::page_dimensions(paper, turned)) {}

    array document::rect_of(const sc::rect &where) const {
        if (where.width() < 0 || where.height() < 0) throw std::invalid_argument("sc::pdflite: an area with a negative size");
        // PDF's Rect is [left bottom right top], y up; numbers that aren't finite throw here.
        const double height = pages_[current_].height;
        return {where.left(), height - where.bottom(), where.right(), height - where.top()};
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

    void document::add_page() { add_page({width_, height_}); }

    void document::add_page(const sc::size_<sc::distance> &size) {
        check_open();
        check_size(size.width(), size.height());
        pages_.push_back({file_.allocate(), size.width().points(), size.height().points(), pdflite::content{size.height()}, {}, {}});
        current_ = pages_.size() - 1;
    }

    void document::add_page(const sc::page_size paper, const sc::orientation turned) {
        add_page(sc::page_dimensions(paper, turned));
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

    std::string document::standard_font(const sc::standard_font::face which) {
        check_open();
        auto &page = pages_[current_];
        return page_name(page.fonts, which, [&] { if (!fonts_.contains(which)) fonts_[which] = file_.allocate(); },
                         "F", page.fonts.size() + page.embedded.size() + 1);
    }

    std::string document::font(const pdflite::font &embedded) {
        check_open();
        const void *key = embedded.identity();
        auto &page = pages_[current_];
        // Numbered after the page's built-in fonts too, so the two never share a name.
        return page_name(page.embedded, key, [&] { if (!embedded_.contains(key)) embedded_.emplace(key, written_font{embedded, file_.allocate()}); },
                         embedded.prefix(), page.fonts.size() + page.embedded.size() + 1);
    }

    std::string document::graphics_state(const double fill_alpha, const double stroke_alpha) {
        check_open();
        if (!(fill_alpha >= 0 && fill_alpha <= 1) || !(stroke_alpha >= 0 && stroke_alpha <= 1))
            throw std::invalid_argument("sc::pdflite: an alpha outside 0 to 1");
        const std::pair key{fill_alpha, stroke_alpha};
        auto &page = pages_[current_];
        return page_name(page.states, key, [&] { if (!states_.contains(key)) states_[key] = file_.allocate(); }, "GS", page.states.size() + 1);
    }

    std::string document::image(const pdflite::image &picture) {
        check_open();
        const void *key = picture.identity();
        auto &page = pages_[current_];
        return page_name(page.images, key, [&] { if (!images_.contains(key)) images_.emplace(key, written_image{picture.clone(), file_.allocate()}); },
                         picture.prefix(), page.images.size() + 1);
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

    void document::link(const sc::rect &where, const std::string_view url) {
        check_open();
        if (url.empty()) throw std::invalid_argument("sc::pdflite: a link to an empty URI");
        pages_[current_].links.push_back({rect_of(where), string{ascii_uri(url)}, 0, 0});
    }

    void document::link(const sc::rect &where, const std::size_t page, const sc::distance top) {
        check_open();
        if (page == 0) throw std::out_of_range("sc::pdflite: pages count from 1");
        number(top); // throws when it isn't finite
        pages_[current_].links.push_back({rect_of(where), std::nullopt, page, top});
    }

    void document::bookmark(const std::string_view title, const std::size_t page, const sc::distance top, const int level) {
        check_open();
        if (page == 0) throw std::out_of_range("sc::pdflite: pages count from 1");
        number(top);
        outline_.push_back({string::text(title), page, top, std::max(0, level)});
    }

    array document::destination(const std::size_t page, const double top) const {
        return {reference{pages_[page - 1].id}, name{"XYZ"}, 0, pages_[page - 1].height - top, nullptr}; // the target page's height
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

    void document::protect(const protection &settings) {
        check_open();
        encryption_ = std::make_shared<const encryption>(settings);
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

        // Encryption first: everything written after it is encrypted, but its own dictionary.
        std::uint32_t encrypt = 0;
        if (encryption_) {
            encrypt = file_.allocate();
            file_.encrypt(encryption_, encrypt);
            file_.write(encrypt, encryption_->dictionary());
        }
        array kids;
        for (auto &page: pages_) {
            const std::uint32_t contents = file_.allocate();
            file_.write_stream(contents, {}, page.content.text());
            dictionary resources;
            if (!page.fonts.empty() || !page.embedded.empty()) {
                dictionary fonts;
                for (const auto &[which, resource]: page.fonts) fonts.add(resource, reference{fonts_.at(which)});
                for (const auto &[key, resource]: page.embedded) fonts.add(resource, reference{embedded_.at(key).id});
                resources.add("Font", fonts);
            }
            if (!page.images.empty()) {
                dictionary xobjects;
                for (const auto &[key, resource]: page.images) xobjects.add(resource, reference{images_.at(key).id});
                resources.add("XObject", xobjects);
            }
            if (!page.states.empty()) {
                dictionary states;
                for (const auto &[key, resource]: page.states) states.add(resource, reference{states_.at(key)});
                resources.add("ExtGState", states);
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
            dictionary entry{{"Type", name{"Page"}}, {"Parent", reference{pages_id_}}, {"MediaBox", array{0, 0, page.width, page.height}},
                             {"Resources", resources}, {"Contents", reference{contents}}};
            if (!annotations.empty()) entry.add("Annots", annotations);
            file_.write(page.id, entry);
            kids.push_back(reference{page.id});
            page.content = pdflite::content{page.height}; // written: its text isn't needed any more
        }
        file_.write(pages_id_, dictionary{{"Type", name{"Pages"}}, {"Kids", kids}, {"Count", pages_.size()}});

        // The standard Type 1 fonts (9.6.2.2): Symbol and ZapfDingbats, with their own encodings,
        // aren't among sc::standard_font's faces.
        for (const auto &[which, id]: fonts_)
            file_.write(id, dictionary{{"Type", name{"Font"}}, {"Subtype", name{"Type1"}}, {"BaseFont", name{sc::standard_font::name(which)}},
                                       {"Encoding", name{"WinAnsiEncoding"}}});

        for (const auto &[alphas, id]: states_)
            file_.write(id, dictionary{{"Type", name{"ExtGState"}}, {"ca", alphas.first}, {"CA", alphas.second}});

        for (const auto &[key, written]: images_) written.picture->write(file_, written.id);
        // Last: the subsets hold every glyph the pages used.
        for (const auto &[key, written]: embedded_) written.embedded.write(file_, written.id);

        const std::uint32_t outlines = write_outline();
        const std::uint32_t catalog = file_.allocate();
        dictionary root{{"Type", name{"Catalog"}}, {"Pages", reference{pages_id_}}};
        // AES-256 (revision 6) in a PDF 1.7 file: Adobe's extension level 8, as Acrobat writes it.
        if (encryption_)
            root.add("Extensions", dictionary{{"ADBE", dictionary{{"BaseVersion", name{"1.7"}}, {"ExtensionLevel", 8}}}});
        if (outlines) root.add("Outlines", reference{outlines}).add("PageMode", name{"UseOutlines"});
        file_.write(catalog, root);

        const auto now = string::date(std::chrono::system_clock::now());
        if (!info_.contains("Producer")) info_.insert_or_assign("Producer", string::text("simply-cpp-pdflite"));
        dictionary information;
        for (const auto &[key, value]: info_) information.add(key, value);
        information.add("CreationDate", now).add("ModDate", now);
        const std::uint32_t information_id = file_.allocate();
        file_.write(information_id, information);

        file_.finish(catalog, information_id, encrypt);
    }
}
