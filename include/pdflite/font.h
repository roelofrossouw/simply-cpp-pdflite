#ifndef SC_PDFLITE_FONT_H
#define SC_PDFLITE_FONT_H

#include "resource.h"

#include <distance.h>
#include <font.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

// sc-pdflite's embedded fonts (ISO 32000-1, 9.7 and 9.9): any font FreeType reads, embedded as a
// subset of the glyphs the document uses.
namespace sc::pdflite {
    // A font embedded in the document: read by FreeType, written as a Type0 font (Identity-H, two
    // bytes a glyph) over a CIDFontType2 whose TrueType program is made of just the glyphs used,
    // their outlines as FreeType gives them: a variable font's at the instance asked for (so bold is
    // bold), composite glyphs flattened, a CFF font's cubic curves turned into TrueType's quadratic
    // ones. It has the tables PDF asks for (head, hhea, maxp, loca, glyf, hmtx, post) and no
    // hinting. A ToUnicode map lets readers copy and search the text; widths are the font's own.
    //
    // Text is put into glyphs with encode(), which also records them for the subset, so a font
    // is written by the document after everything has been drawn. A character the font has no glyph
    // for is drawn as its .notdef (usually a box); covers() tells. No kerning or ligatures. Copies
    // share the font, and a document writes it once.
    class font : public resource {
    public:
        // A font file, and the face in it (a collection's or a variable font's named instance, as
        // FreeType and fontconfig number them). Throws std::runtime_error when FreeType can't read it.
        [[nodiscard]] static font load(const std::filesystem::path &file, long index = 0);

        // The installed font closest to spec, as fontconfig finds it (sc-graphics' sc::font::face()):
        // sc::pdflite::font{sc::font{"sans-serif", 12, 700}}. Throws std::runtime_error when there's none.
        explicit font(const sc::font &spec);

        // Its PostScript name ("DejaVuSans-Bold").
        [[nodiscard]] const std::string &name() const;

        // The bytes content::show() writes for UTF-8 text in this font: each character's glyph,
        // two bytes, recorded for the subset. Throws std::invalid_argument on malformed UTF-8.
        [[nodiscard]] std::string encode(std::string_view utf8) const;

        // Whether it has a glyph for every character of the text.
        [[nodiscard]] bool covers(std::string_view utf8) const;

        // How far the text advances at size, and the font's vertical metrics there.
        [[nodiscard]] sc::distance width(std::string_view utf8, sc::distance size) const;

        [[nodiscard]] sc::distance ascender(sc::distance size) const;

        [[nodiscard]] sc::distance descender(sc::distance size) const; // positive: below the baseline

        // The cap and x heights: OS/2's, or else how high an H and an x rise.
        [[nodiscard]] sc::distance cap_height(sc::distance size) const;

        [[nodiscard]] sc::distance x_height(sc::distance size) const;

        [[nodiscard]] std::string_view category() const override { return "Font"; }

        [[nodiscard]] std::string_view prefix() const override { return "F"; }

        // The Type0 font as object id, and its descendant, descriptor, subset and ToUnicode map.
        void write(file &out, std::uint32_t id) const override;

        // What copies share: one font, and the glyphs used of it.
        [[nodiscard]] const void *identity() const { return data_.get(); }

    private:
        font() = default;

        struct data;
        std::shared_ptr<data> data_;
    };
}

#endif //SC_PDFLITE_FONT_H
