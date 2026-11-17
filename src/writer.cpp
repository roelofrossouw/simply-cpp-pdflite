#include <sc/pdflite/writer.h>

#include <sc/pdflite/pdflite.h>
#include <sc/graphics/standard_font.h>
#include <sc/core/utf8.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace sc::pdflite {
    struct writer::impl {
        pdflite::document document;
        bool builtin = false;
        // One pdflite::font serves every size of a face: keyed by family, weight, italic and style.
        std::map<std::tuple<std::string, int, bool, std::string>, pdflite::font> fonts;
        // Each bitmap read once, by its bytes (copies share them), held so their address isn't another's.
        std::map<const std::string *, std::pair<sc::bitmap, std::shared_ptr<const pdflite::image> > > bitmaps;
        std::map<std::filesystem::path, std::shared_ptr<const pdflite::image> > files;

        impl(const std::filesystem::path &filename, const page_size size, const orientation turned) : document(filename, size, turned) {
        }

        void check_open() const {
            if (document.closed()) throw std::logic_error("sc::pdflite::writer: drawing after close()");
        }

        pdflite::content &content() {
            check_open();
            return document.page_content();
        }

        // The installed font for spec; throws std::runtime_error when fontconfig finds none.
        const pdflite::font &font(const sc::font &spec) {
            const auto key = std::tuple{spec.family, spec.weight, spec.italic, spec.style};
            if (const auto found = fonts.find(key); found != fonts.end()) return found->second;
            return fonts.emplace(key, pdflite::font{spec}).first->second;
        }

        // Makes what follows (up to the next restore) paint at these alphas; opaque needs nothing.
        void set_alpha(pdflite::content &page, const double fill_alpha, const double stroke_alpha) {
            if (fill_alpha >= 1 && stroke_alpha >= 1) return;
            page.state(document.graphics_state(std::clamp(fill_alpha, 0.0, 1.0), std::clamp(stroke_alpha, 0.0, 1.0)));
        }

        // Fills (by the fill's rule) and/or strokes shape; whichever is none is left out.
        void paint(const sc::appearance &look, const sc::path &shape) {
            const auto &stroke = look.stroke();
            const auto &fill = look.fill();
            auto &page = content();
            const bool stroked = stroke.visible(), filled = fill.visible();
            page.save();
            set_alpha(page, filled ? fill.colour().alpha() : 1, stroked ? stroke.colour().alpha() : 1);
            if (filled) page.fill_colour(fill.colour().red(), fill.colour().green(), fill.colour().blue());
            if (stroked) {
                page.line_width(stroke.width());
                page.stroke_colour(stroke.colour().red(), stroke.colour().green(), stroke.colour().blue());
            }
            page.path(shape);
            page.paint(filled, stroked, fill.rule() == fill_rule::even_odd);
            page.restore();
        }

        // A JPEG or PNG, by its first bytes.
        static std::shared_ptr<const pdflite::image> picture(std::string bytes) {
            if (bytes.starts_with("\x89PNG")) return std::make_shared<pdflite::png>(std::move(bytes));
            if (bytes.starts_with("\xFF\xD8")) return std::make_shared<pdflite::jpeg>(std::move(bytes));
            throw std::runtime_error("sc::pdflite::writer: an image that's neither a JPEG nor a PNG");
        }
    };

    writer::writer(const std::filesystem::path &filename, const page_size size, const orientation turned)
        : impl_(std::make_unique<impl>(filename, size, turned)) {
    }

    writer::writer(const std::filesystem::path &filename, const page_size size, const orientation turned, const protection &locked)
        : impl_(std::make_unique<impl>(filename, size, turned)) {
        if (locked.user_password.empty() && locked.owner_password.empty()) return;
        impl_->document.protect(locked);
    }

    writer::~writer() = default;

    writer::writer(writer &&) noexcept = default;

    writer &writer::operator=(writer &&) noexcept = default;

    void writer::close() {
        impl_->document.close();
    }

    void writer::new_page() {
        impl_->check_open();
        impl_->document.add_page();
    }

    void writer::set_page(const unsigned page) {
        impl_->check_open();
        if (page == 0) throw std::out_of_range("sc::pdflite::writer::set_page: pages count from 1");
        while (impl_->document.page_count() < page) impl_->document.add_page();
        impl_->document.set_page(page);
    }

    unsigned writer::page() const {
        return static_cast<unsigned>(impl_->document.page());
    }

    unsigned writer::page_count() const {
        return static_cast<unsigned>(impl_->document.page_count());
    }

    double writer::page_width() const {
        return impl_->document.size().width().points();
    }

    double writer::page_height() const {
        return impl_->document.size().height().points();
    }

    namespace {
        // UTF-8 text in pieces: Windows-1252's (as its bytes) and the rest (as UTF-8), in order.
        struct piece {
            std::string text; // Windows-1252 bytes when encoded, else UTF-8
            std::string utf8;
            bool encoded;
        };

        std::vector<piece> pieces_of(const std::string_view text) {
            std::vector<piece> out;
            for (std::size_t i = 0; i < text.size();) {
                const auto [code, length, valid] = utf8::decode(text, i);
                const auto byte = standard_font::encode(code);
                const bool encoded = byte.has_value();
                if (out.empty() || out.back().encoded != encoded) out.push_back({{}, {}, encoded});
                if (encoded) out.back().text += *byte;
                else out.back().text.append(text.substr(i, length));
                out.back().utf8.append(text.substr(i, length));
                i += length;
            }
            return out;
        }
    }

    double writer::measure_width(const text &sample) {
        if (impl_->builtin) {
            if (const auto standard = standard_font::of(sample.font)) {
                double width = 0;
                for (const auto &each: pieces_of(sample.content))
                    width += each.encoded ? standard->width(each.utf8, sample.font.size)
                                          : impl_->font(sample.font).width(each.text, sample.font.size).points();
                return width;
            }
        }
        return impl_->font(sample.font).width(sample.content, sample.font.size).points();
    }

    double writer::measure_x_height(const sc::font &font) {
        if (impl_->builtin)
            if (const auto standard = standard_font::of(font)) return standard->x_height(font.size);
        return impl_->font(font).x_height(font.size).points();
    }

    double writer::measure_cap_height(const sc::font &font) {
        if (impl_->builtin)
            if (const auto standard = standard_font::of(font)) return standard->cap_height(font.size);
        return impl_->font(font).cap_height(font.size).points();
    }

    void writer::builtin_fonts(const bool on) { impl_->builtin = on; }

    bool writer::builtin_fonts() const { return impl_->builtin; }

    void writer::image_resolution(const double pixels_per_inch) { impl_->document.image_resolution(pixels_per_inch); }

    void writer::paint_text(const text &sample, const sc::fill &fill) {
        auto &content = impl_->content();
        auto &document = impl_->document;
        content.save();
        impl_->set_alpha(content, fill.colour().alpha(), 1);
        content.fill_colour(fill.colour().red(), fill.colour().green(), fill.colour().blue());
        content.begin_text();
        const auto standard = impl_->builtin ? standard_font::of(sample.font) : std::nullopt;
        if (standard) {
            // The built-in font for what Windows-1252 has, placed by Adobe's widths; the installed
            // font (embedded) for the rest.
            double x = sample.position.x();
            for (const auto &each: pieces_of(sample.content)) {
                if (each.encoded) {
                    content.font(document.standard_font(standard->which()), sample.font.size);
                    content.text_position(x, sample.position.y());
                    content.show(each.text);
                    x += standard->width(each.utf8, sample.font.size);
                } else {
                    const auto &installed = impl_->font(sample.font);
                    content.font(document.font(installed), sample.font.size);
                    content.text_position(x, sample.position.y());
                    content.show(installed.encode(each.text));
                    x += installed.width(each.text, sample.font.size).points();
                }
            }
        } else {
            const auto &installed = impl_->font(sample.font);
            content.font(document.font(installed), sample.font.size);
            content.text_position(sample.position.x(), sample.position.y());
            content.show(installed.encode(sample.content));
        }
        content.end_text();
        content.restore();
    }

    void writer::paint_line(const line &shape, const sc::stroke &stroke) {
        impl_->paint(stroke, to_path(shape));
    }

    void writer::paint_rounded_rect(const rounded_rect &shape, const sc::appearance &look) {
        impl_->paint(look, to_path(shape));
    }

    void writer::paint_circle(const circle &shape, const sc::appearance &look) {
        impl_->paint(look, to_path(shape));
    }

    void writer::paint_polygon(const polygon &shape, const sc::appearance &look) {
        impl_->paint(look, to_path(shape));
    }

    void writer::draw_image(const std::filesystem::path &filename, const rect &pos) {
        impl_->check_open();
        auto found = impl_->files.find(filename);
        if (found == impl_->files.end()) {
            std::ifstream in{filename, std::ios::binary};
            if (!in) throw std::runtime_error("sc::pdflite::writer: can't read the image " + filename.string());
            std::string bytes{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
            found = impl_->files.emplace(filename, impl::picture(std::move(bytes))).first;
        }
        const auto &picture = *found->second;
        const auto natural = picture.size();
        if (natural.width() <= 0 || natural.height() <= 0) throw std::runtime_error("sc::pdflite::writer: can't read the image " + filename.string());
        const double scale = std::min(pos.width() / natural.width(), pos.height() / natural.height());
        const double w = natural.width() * scale, h = natural.height() * scale;
        const rect where{pos.left() + (pos.width() - w) / 2, pos.top() + (pos.height() - h) / 2, w, h};
        impl_->content().draw(impl_->document.image(picture, sc::size_<distance>{w, h}), where);
    }

    void writer::paint_bitmap(const bitmap &picture, const transform &placement) {
        impl_->check_open();
        auto found = impl_->bitmaps.find(&picture.bytes());
        if (found == impl_->bitmaps.end())
            found = impl_->bitmaps.emplace(&picture.bytes(), std::pair{picture, impl::picture(picture.bytes())}).first;
        // Its unit square's sides, as drawn: what a lower resolution goes by.
        const sc::size_<distance> shown{std::hypot(placement.a(), placement.b()), std::hypot(placement.c(), placement.d())};
        impl_->content().draw(impl_->document.image(*found->second.second, shown), placement);
    }

    void writer::paint_path(const path &shape, const sc::appearance &look) {
        impl_->paint(look, shape);
    }

    void writer::push_clip(const path &shape) {
        auto &content = impl_->content();
        content.save();
        // An empty shape clips everything away (html2pdf asks for one where nothing of an area is
        // left on a page): a rectangle of no size, since PDF clips only to a path.
        if (shape.empty()) content.rectangle({}, {}, {}, {});
        else content.path(shape);
        content.clip(); // nonzero, as canvas::push_clip() clips
        content.end_path();
    }

    void writer::pop_clip() {
        impl_->content().restore();
    }

    void writer::add_link(const std::string &url, const rect &pos) {
        impl_->check_open();
        impl_->document.link(pos, url);
    }

    void writer::add_link(const unsigned page, const rect &pos, const double top) {
        impl_->check_open();
        if (page == 0) throw std::out_of_range("sc::pdf::add_link: pages count from 1");
        impl_->document.link(pos, static_cast<std::size_t>(page), top);
    }

    void writer::add_outline(const std::string &title, const unsigned page, const double top, const int level) {
        impl_->check_open();
        if (page == 0) throw std::out_of_range("sc::pdf::add_outline: pages count from 1");
        impl_->document.bookmark(title, page, top, std::max(0, level));
    }

    void writer::metadata(const std::string &key, const std::string &value) {
        impl_->check_open();
        impl_->document.info(key, value);
    }
}
