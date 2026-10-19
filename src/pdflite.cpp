#include "pdflite.h"
#include <standard_font.h>

#include <algorithm>
#include <array>
#include <chrono>
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

    void document::close() {
        if (file_.finished()) return;
        for (std::size_t i = 0; i < pages_.size(); ++i)
            if (!pages_[i].content.complete())
                throw std::logic_error("sc::pdflite: page " + std::to_string(i + 1) + "'s content isn't complete (an open q, text object or path)");

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
            file_.object(page.id, "<< /Type /Page /Parent " + reference(pages_id_) + " /MediaBox " + media_box +
                                  " /Resources " + resources + " /Contents " + reference(contents) + " >>");
            kids += (kids.empty() ? "" : " ") + reference(page.id);
            page.content = {}; // written: its text isn't needed any more
        }
        file_.object(pages_id_, "<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pages_.size()) + " >>");

        for (const auto &[base_font, id]: fonts_)
            file_.object(id, "<< /Type /Font /Subtype /Type1 /BaseFont " + name(base_font) + " /Encoding /WinAnsiEncoding >>");

        const std::uint32_t catalog = file_.allocate();
        file_.object(catalog, "<< /Type /Catalog /Pages " + reference(pages_id_) + " >>");

        const auto now = date(std::chrono::system_clock::now());
        if (!info_.contains("Producer")) info_["Producer"] = text_string("simply-cpp-pdf");
        std::string entries;
        for (const auto &[key, value]: info_) entries += " " + name(key) + " " + value;
        const std::uint32_t information = file_.allocate();
        file_.object(information, "<<" + entries + " /CreationDate " + now + " /ModDate " + now + " >>");

        file_.finish(catalog, information);
    }
}
