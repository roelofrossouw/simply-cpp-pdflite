#ifndef SC_PDFLITE_SVG_H
#define SC_PDFLITE_SVG_H

#include "image.h"

#include <drawing.h>

#include <filesystem>
#include <memory>
#include <string_view>

// sc-pdflite's SVG drawings (ISO 32000-1, 8.10: form XObjects).
namespace sc::pdflite {
    // An SVG drawing, read by sc-graphics (sc::drawing), written as a form XObject: vectors, so it
    // stays sharp at any size, drawn with content::draw() like any picture. Its shapes keep their
    // strokes, fills (nonzero or even-odd), clips and transparency (graphics states); its text is
    // written in PDF's built-in fonts (sc::standard_font::of() the label's font, else Helvetica of
    // its weight and slant), a character they don't have as '?'; a PNG or JPEG in it becomes an image
    // in the form. What sc::drawing doesn't read (see drawing.h) isn't drawn; a gradient fills with
    // its middle colour, as the other PDF engines draw it.
    class svg : public image {
    public:
        // An SVG file; throws std::runtime_error when it can't be read or parsed.
        [[nodiscard]] static svg load(const std::filesystem::path &path);

        // SVG text.
        [[nodiscard]] static svg parse(std::string_view text);

        // A drawing of its own (sc::drawing can be drawn on, too).
        explicit svg(sc::drawing drawing);

        // Its page's size, in the drawing's units (CSS pixels for an SVG).
        [[nodiscard]] double width() const override;

        [[nodiscard]] double height() const override;

        [[nodiscard]] const sc::drawing &drawing() const { return *drawing_; }

        [[nodiscard]] std::string_view category() const override { return "XObject"; }

        [[nodiscard]] std::string_view prefix() const override { return "Fm"; }

        // Writes the form, and the fonts and images it uses as objects of their own.
        void write(file &out, std::uint32_t id) const override;

        [[nodiscard]] std::shared_ptr<const image> clone() const override { return std::make_shared<svg>(*this); }

        [[nodiscard]] const void *identity() const override { return drawing_.get(); }

    private:
        std::shared_ptr<const sc::drawing> drawing_;
    };
}

#endif //SC_PDFLITE_SVG_H
