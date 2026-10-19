// Writes a page of text in embedded fonts with sc-pdflite: fonts found by fontconfig as an
// sc::font asks (sans-serif, serif, monospace, any weight), written as subsets of the glyphs
// used, with a ToUnicode map so the text can be copied and searched. A title centred by measuring
// it, other alphabets, weights, and a check of what a font covers. Each line shows a call and what
// it returned.

#include <filesystem>
#include <iostream>
#include <string>

#include <params.h>
#include <sc.h>

#include <distance.h>
#include <pdflite.h>

// The units by their plain names: mm(10), inch(1), pdf.size().in<mm>().
using namespace sc::dist;

namespace {
    // A line in an embedded font, its baseline starting at (x, y).
    void write(sc::pdflite::document &pdf, const sc::pdflite::font &font, const sc::distance size, const sc::distance x,
               const sc::distance y, const std::string &text) {
        auto &page = pdf.page_content();
        page.begin_text();
        page.font(pdf.font(font), size);
        page.text_position(x, y);
        page.show(font.encode(text));
        page.end_text();
    }
}

int main(int argc, char **argv) {
    const sc::params args{argc, argv, {
        {"name", "sc-pdflite-fonts"},
        {"description", "Writes a page of text in embedded fonts with sc-pdflite: the installed fonts fontconfig finds "
                        "(sans-serif, serif, monospace, several weights), each embedded as a subset of the glyphs used."},
        {"positional", {
            {"output", {{"default", (std::filesystem::temp_directory_path() / "sc-pdflite-fonts.pdf").string()},
                        {"help", "the PDF to write"}}}}}}};

    try {
        const std::filesystem::path output = args.as<std::string>("output");
        sc::console::title("simply-cpp pdflite: embedded fonts");
        sc::timer sw;

        // [readme]
        sc::pdflite::document pdf{output, sc::page_size::A4};

        sc::console::heading("A font as an sc::font asks for it: fontconfig finds it, FreeType reads it");
        const sc::pdflite::font title{sc::font::bold("sans-serif", 26)};
        SC_SHOW(title.name()); // its PostScript name
        const std::string heading = "Embedded fonts, as subsets";
        const auto width = title.width(heading, 26);
        SC_SHOW(width);
        write(pdf, title, 26, (pdf.size().width() - width) / 2, mm(30), heading); // centred

        sc::console::heading("Text in it is glyphs: encode() picks them and records them for the subset");
        const sc::pdflite::font serif{sc::font{"serif", 13}};
        SC_SHOW(serif.name());
        write(pdf, serif, 13, mm(20), mm(45), "Only the glyphs this page uses go into the file, and a ToUnicode map lets");
        write(pdf, serif, 13, mm(20), mm(51), "readers copy and search the text: café, naïve, “quotes”, € 12,50.");

        sc::console::heading("Other alphabets, where the font has them (covers())");
        const sc::pdflite::font sans{sc::font{"sans-serif", 14}};
        const std::string greek = "Καλημέρα κόσμε", cyrillic = "Привет, мир";
        SC_SHOW(sans.covers(greek));
        SC_SHOW(sans.covers(cyrillic));
        write(pdf, sans, 14, mm(20), mm(66), "Greek: " + greek);
        write(pdf, sans, 14, mm(20), mm(74), "Cyrillic: " + cyrillic);

        sc::console::heading("Weights: each the face fontconfig finds for it");
        sc::distance y = mm(92);
        for (const int weight: {300, 400, 600, 800}) {
            const sc::pdflite::font face{sc::font{"sans-serif", 16, weight}};
            SC_SHOW(face.name());
            write(pdf, face, 16, mm(20), y, "Weight " + std::to_string(weight) + ": " + face.name());
            y = y + mm(9);
        }

        sc::console::heading("A fixed-width face, measured");
        const sc::pdflite::font mono{sc::font{"monospace", 11}};
        SC_SHOW(mono.width("iiii", 11) == mono.width("MMMM", 11)); // every glyph as wide
        write(pdf, mono, 11, mm(20), mm(135), "for (const auto &glyph: subset) write(glyph);");

        pdf.info("Title", "sc-pdflite: embedded fonts");
        pdf.info("Author", "Roelof Rossouw");
        pdf.info("Subject", "Installed fonts embedded as subsets: sans-serif, serif, monospace, weights and other alphabets");
        pdf.info("Creator", "sc-pdflite-fonts");
        SC_STEP(pdf.close());
        SC_SHOW(std::filesystem::file_size(output)); // bytes: subsets, not whole fonts
        // [/readme]

        sc::console::output() << "\nThe result is " << output.string() << "\nAll of that took " << sw << ".\n";
    } catch (const std::exception &error) {
        std::cerr << "sc-pdflite-fonts: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
