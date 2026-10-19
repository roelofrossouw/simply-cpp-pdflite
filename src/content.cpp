#include "pdflite.h"
#include <stdexcept>
#include <string>

namespace sc::pdflite {
    namespace {
        std::string colour(const double red, const double green, const double blue) {
            for (const double channel: {red, green, blue})
                if (!(channel >= 0 && channel <= 1)) throw std::invalid_argument("sc::pdflite: a colour channel outside 0 to 1");
            return number(red) + " " + number(green) + " " + number(blue);
        }
    }

    void content::op(const std::string_view line) {
        text_.append(line);
        text_ += '\n';
    }

    void content::outside_text(const std::string_view what) const {
        if (in_text_) throw std::logic_error("sc::pdflite: " + std::string{what} + " inside a text object (BT ... ET)");
        if (in_path_) throw std::logic_error("sc::pdflite: " + std::string{what} + " before the path is painted or ended");
    }

    void content::save() {
        outside_text("q");
        op("q");
        saved_.push_back(font_set_);
    }

    void content::restore() {
        outside_text("Q");
        if (saved_.empty()) throw std::logic_error("sc::pdflite: Q without its q");
        op("Q");
        font_set_ = saved_.back(); // the text state is part of the graphics state Q restores
        saved_.pop_back();
    }

    void content::transform(const double a, const double b, const double c, const double d, const double e, const double f) {
        outside_text("cm");
        op(number(a) + " " + number(b) + " " + number(c) + " " + number(d) + " " + number(e) + " " + number(f) + " cm");
    }

    void content::line_width(const double width) {
        if (in_path_) throw std::logic_error("sc::pdflite: w before the path is painted or ended");
        if (!(width >= 0)) throw std::invalid_argument("sc::pdflite: a negative line width");
        op(number(width) + " w");
    }

    void content::fill_colour(const double red, const double green, const double blue) {
        if (in_path_) throw std::logic_error("sc::pdflite: rg before the path is painted or ended");
        op(colour(red, green, blue) + " rg");
    }

    void content::stroke_colour(const double red, const double green, const double blue) {
        if (in_path_) throw std::logic_error("sc::pdflite: RG before the path is painted or ended");
        op(colour(red, green, blue) + " RG");
    }

    void content::state(const std::string_view resource) {
        if (in_path_) throw std::logic_error("sc::pdflite: gs before the path is painted or ended");
        op(name(resource) + " gs");
    }

    void content::path_step(const std::string_view what) {
        if (in_text_) throw std::logic_error("sc::pdflite: " + std::string{what} + " inside a text object (BT ... ET)");
        if (clipping_) throw std::logic_error("sc::pdflite: " + std::string{what} + " between W and the painting operator");
    }

    void content::move(const double x, const double y) {
        path_step("m");
        op(number(x) + " " + number(y) + " m");
        in_path_ = true;
    }

    void content::line(const double x, const double y) {
        path_step("l");
        if (!in_path_) throw std::logic_error("sc::pdflite: l without a current point (m first)");
        op(number(x) + " " + number(y) + " l");
    }

    void content::curve(const double x1, const double y1, const double x2, const double y2, const double x3, const double y3) {
        path_step("c");
        if (!in_path_) throw std::logic_error("sc::pdflite: c without a current point (m first)");
        op(number(x1) + " " + number(y1) + " " + number(x2) + " " + number(y2) + " " + number(x3) + " " + number(y3) + " c");
    }

    void content::close_path() {
        path_step("h");
        if (!in_path_) throw std::logic_error("sc::pdflite: h without a path");
        op("h");
    }

    void content::rectangle(const double x, const double y, const double width, const double height) {
        path_step("re");
        op(number(x) + " " + number(y) + " " + number(width) + " " + number(height) + " re");
        in_path_ = true;
    }

    void content::paint(const std::string_view line) {
        if (!in_path_) throw std::logic_error("sc::pdflite: " + std::string{line} + " without a path to paint");
        op(line);
        in_path_ = false;
        clipping_ = false;
    }

    void content::fill(const bool even_odd) { paint(even_odd ? "f*" : "f"); }

    void content::stroke() { paint("S"); }

    void content::fill_and_stroke(const bool even_odd) { paint(even_odd ? "B*" : "B"); }

    void content::end_path() { paint("n"); }

    void content::clip(const bool even_odd) {
        if (!in_path_) throw std::logic_error("sc::pdflite: W without a path");
        if (clipping_) throw std::logic_error("sc::pdflite: W twice on one path");
        op(even_odd ? "W*" : "W");
        clipping_ = true;
    }

    void content::begin_text() {
        outside_text("BT");
        op("BT");
        in_text_ = true;
    }

    void content::end_text() {
        if (!in_text_) throw std::logic_error("sc::pdflite: ET without BT");
        op("ET");
        in_text_ = false;
    }

    void content::font(const std::string_view resource, const double size) {
        if (in_path_) throw std::logic_error("sc::pdflite: Tf before the path is painted or ended");
        op(name(resource) + " " + number(size) + " Tf");
        font_set_ = true;
    }

    void content::text_position(const double x, const double y) {
        if (!in_text_) throw std::logic_error("sc::pdflite: Td outside a text object (BT first)");
        op(number(x) + " " + number(y) + " Td");
    }

    void content::show(const std::string_view bytes) {
        if (!in_text_) throw std::logic_error("sc::pdflite: Tj outside a text object (BT first)");
        if (!font_set_) throw std::logic_error("sc::pdflite: Tj before a font is set (Tf)");
        op(literal(bytes) + " Tj");
    }
}
