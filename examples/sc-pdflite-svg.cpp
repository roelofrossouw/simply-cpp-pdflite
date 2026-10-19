// Writes a page of SVG drawings with sc-pdflite: a scene from an SVG file (shapes, a transform,
// transparency, an even-odd fill, a clip and text) drawn large, small, mirrored and turned, and a
// badge from SVG text. Each is a form of vectors, written once and sharp at any size. Each line
// shows a call and what it returned.
//
// The drawing, examples/resource/drawing.svg, was made for this demo.

#include <filesystem>
#include <iostream>
#include <string>

#include <params.h>
#include <sc.h>

#include <distance.h>
#include <pdflite.h>
#include <standard_font.h>

// The units by their plain names: mm(10), inch(1), pdf.size().in<mm>().
using namespace sc::dist;

namespace {
    void caption(sc::pdflite::document &pdf, const sc::rect &box, const std::string &text) {
        const sc::standard_font font{sc::standard_font::face::helvetica};
        auto &page = pdf.page_content();
        page.begin_text();
        page.font(pdf.standard_font(font.which()), 10);
        page.text_position(box.left() + (box.width() - font.width(text, 10)) / 2, box.bottom() + 14);
        page.show(text);
        page.end_text();
    }

    sc::transform filling(const sc::rect &box) { return {box.width(), 0, 0, box.height(), box.left(), box.top()}; }
}

int main(int argc, char **argv) {
    const sc::params args{argc, argv, {
        {"name", "sc-pdflite-svg"},
        {"description", "Writes a page of SVG drawings with sc-pdflite: a scene from an SVG file drawn large, small, "
                        "mirrored and turned, and a badge from SVG text; vectors, sharp at any size."},
        {"options", {
            {"svg", {{"value", "FILE"}, {"help", "the SVG to draw (default: the demo's drawing.svg)"}}}}},
        {"positional", {
            {"output", {{"default", (std::filesystem::temp_directory_path() / "sc-pdflite-svg.pdf").string()},
                        {"help", "the PDF to write"}}}}}}};

    try {
        const std::filesystem::path output = args.as<std::string>("output");
        std::filesystem::path file = args.as<std::string>("svg", SC_PDFLITE_EXAMPLE_INSTALLED "/drawing.svg");
        if (!args.lookup("svg") && !std::filesystem::exists(file)) file = SC_PDFLITE_EXAMPLE_SOURCE "/drawing.svg";
        sc::console::title("simply-cpp pdflite: SVG drawings, as vectors");
        sc::timer sw;

        // [readme]
        sc::pdflite::document pdf{output, sc::page_size::A4};
        auto &page = pdf.page_content();

        sc::console::heading("An SVG file, read by sc-graphics: a form of vectors, drawn like a picture");
        const auto scene = sc::pdflite::svg::load(file);
        SC_SHOW(scene.size()); // the drawing's own units
        SC_SHOW(scene.drawing().items().size());
        const auto name = pdf.image(scene);
        SC_SHOW(name);

        const sc::distance width = mm(170);
        const sc::rect large{mm(20), mm(20), width, width * scene.size().height() / scene.size().width()};
        SC_STEP(page.draw(name, large));
        caption(pdf, large, "Large: vectors stay sharp");

        sc::console::heading("The same form, small, mirrored and turned: written once");
        const sc::rect small{mm(20), mm(170), mm(40), mm(30)};
        SC_STEP(page.draw(name, small));
        caption(pdf, small, "Small");
        const sc::rect mirrored{mm(80), mm(170), mm(40), mm(30)};
        SC_STEP(page.draw(name, filling(mirrored).then(sc::transform::flip_horizontal(mirrored.center().x()))));
        caption(pdf, mirrored, "Mirrored");
        const sc::rect turned{mm(140), mm(170), mm(40), mm(30)};
        SC_STEP(page.draw(name, filling(turned).then(sc::transform::rotate(-15, turned.center()))));
        caption(pdf, turned, "Turned");

        sc::console::heading("SVG text, parsed: a badge with transparency and text");
        const auto badge = sc::pdflite::svg::parse(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="240" height="80">
            <rect x="4" y="4" width="232" height="72" rx="36" fill="#264653"/>
            <circle cx="44" cy="40" r="26" fill="#e9c46a" fill-opacity="0.85"/>
            <text x="84" y="50" font-family="Helvetica" font-weight="bold" font-size="24" fill="#ffffff">sc-pdflite</text>
        </svg>)svg");
        const sc::rect badge_box{mm(65), mm(225), mm(80), mm(80) * 80 / 240};
        SC_STEP(page.draw(pdf.image(badge), badge_box));
        caption(pdf, badge_box, "From SVG text");

        pdf.info("Title", "sc-pdflite: SVG drawings");
        pdf.info("Author", "Roelof Rossouw");
        pdf.info("Subject", "SVG drawings as vectors: large, small, mirrored, turned, and a badge from SVG text");
        pdf.info("Creator", "sc-pdflite-svg");
        SC_STEP(pdf.close());
        SC_SHOW(std::filesystem::file_size(output)); // bytes: the scene is in it once
        // [/readme]

        sc::console::output() << "\nThe result is " << output.string() << "\nAll of that took " << sw << ".\n";
    } catch (const std::exception &error) {
        std::cerr << "sc-pdflite-svg: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
