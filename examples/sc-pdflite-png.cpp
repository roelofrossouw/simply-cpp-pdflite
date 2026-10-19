// Writes a page of PNG images with sc-pdflite over a striped background, so their transparency
// shows: a disc whose edge fades out (RGBA, an alpha channel), the same PNG interlaced, and a
// flower whose palette has a see-through colour; each PNG is told whether it was copied as it is
// or decoded. Each line shows a call and what it returned.
//
// The PNGs in examples/resource are drawn by make-demo-pngs.py, beside them.

#include <filesystem>
#include <iostream>
#include <string>

#include <params.h>
#include <sc.h>

#include <distance.h>
#include <pdflite.h>
#include <standard_font.h>

namespace {
    // Diagonal stripes over area, so what's see-through in a picture shows.
    void stripes(sc::pdflite::content &page, const sc::rect &area) {
        page.save();
        page.rectangle(area.left(), area.top(), area.width(), area.height());
        page.clip();
        page.end_path();
        page.fill_colour(0.85, 0.85, 0.85);
        for (double x = area.left() - area.height(); x < area.right(); x += 16) {
            page.move(x, area.bottom());
            page.line(x + area.height(), area.top());
            page.line(x + area.height() + 8, area.top());
            page.line(x + 8, area.bottom());
            page.close_path();
            page.fill();
        }
        page.restore();
    }

    void caption(sc::pdflite::document &pdf, const sc::rect &box, const std::string &text) {
        const sc::standard_font font{sc::standard_font::face::helvetica};
        auto &page = pdf.page_content();
        page.begin_text();
        page.font(pdf.standard_font(font.which()), 10);
        page.text_position(box.left() + (box.width() - font.width(text, 10)) / 2, box.bottom() + 14);
        page.show(text);
        page.end_text();
    }
}

int main(int argc, char **argv) {
    const sc::params args{argc, argv, {
        {"name", "sc-pdflite-png"},
        {"description", "Writes a page of PNG images with sc-pdflite over stripes, so their transparency shows: an "
                        "alpha channel, the same PNG interlaced, and a palette with a see-through colour."},
        {"options", {
            {"pictures", {{"value", "FOLDER"}, {"help", "where badge.png and the others are (default: where they're installed, else the source's)"}}}}},
        {"positional", {
            {"output", {{"default", (std::filesystem::temp_directory_path() / "sc-pdflite-png.pdf").string()},
                        {"help", "the PDF to write"}}}}}}};

    try {
        const std::filesystem::path output = args.as<std::string>("output");
        std::filesystem::path folder = args.as<std::string>("pictures", SC_PDFLITE_EXAMPLE_INSTALLED);
        if (!args.lookup("pictures") && !std::filesystem::exists(folder / "badge.png")) folder = SC_PDFLITE_EXAMPLE_SOURCE;
        sc::console::title("simply-cpp pdflite: PNG images and their transparency");
        sc::timer sw;

        // [readme]
        sc::pdflite::document pdf{output, sc::page_size::A4};
        auto &page = pdf.page_content();
        stripes(page, sc::rect{sc::dist::mm(15), sc::dist::mm(15), sc::dist::mm(180), sc::dist::mm(200)});

        sc::console::heading("An alpha channel: decoded, its alpha written as a soft mask");
        const auto badge = sc::pdflite::png::load(folder / "badge.png");
        SC_SHOW(badge.size());
        SC_SHOW(badge.colour_type()); // 6: RGB and alpha
        SC_SHOW(badge.copied());      // false: an alpha channel has to be split off
        const sc::rect left{sc::dist::mm(25), sc::dist::mm(25), sc::dist::mm(70), sc::dist::mm(70)};
        SC_STEP(page.draw(pdf.image(badge), left));
        caption(pdf, left, "RGBA: the edge fades out");

        sc::console::heading("Interlaced (Adam7): the passes put back together");
        const auto interlaced = sc::pdflite::png::load(folder / "badge-interlaced.png");
        SC_SHOW(interlaced.interlaced());
        const sc::rect right{sc::dist::mm(115), sc::dist::mm(25), sc::dist::mm(70), sc::dist::mm(70)};
        SC_STEP(page.draw(pdf.image(interlaced), right));
        caption(pdf, right, "The same, interlaced");

        sc::console::heading("A palette with a see-through colour (tRNS)");
        const auto stamp = sc::pdflite::png::load(folder / "stamp.png");
        SC_SHOW(stamp.colour_type()); // 3: a palette
        SC_SHOW(stamp.transparent()); // its colour 0 is see-through
        const sc::rect middle{sc::dist::mm(70), sc::dist::mm(115), sc::dist::mm(70), sc::dist::mm(70)};
        SC_STEP(page.draw(pdf.image(stamp), middle));
        caption(pdf, middle, "A palette, its first colour see-through");

        sc::console::heading("Without transparency, a PNG is copied as it is: nothing decoded");
        const auto gradient = sc::pdflite::png::load(folder / "gradient.png");
        SC_SHOW(gradient.copied()); // true: its compressed data goes in with PNG's own predictors
        const sc::rect below{sc::dist::mm(45), sc::dist::mm(230), sc::dist::mm(120), sc::dist::mm(40)};
        SC_STEP(page.draw(pdf.image(gradient), below));
        caption(pdf, below, "RGB without transparency: copied as it is");

        pdf.info("Title", "sc-pdflite: PNG images");
        pdf.info("Author", "Roelof Rossouw");
        pdf.info("Subject", "PNG images with an alpha channel, interlaced, and a palette with a see-through colour, over stripes");
        pdf.info("Creator", "sc-pdflite-png");
        SC_STEP(pdf.close());
        SC_SHOW(std::filesystem::file_size(output)); // bytes
        // [/readme]

        sc::console::output() << "\nThe result is " << output.string() << "\nAll of that took " << sw << ".\n";
    } catch (const std::exception &error) {
        std::cerr << "sc-pdflite-png: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
