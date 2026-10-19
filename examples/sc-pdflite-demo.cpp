// Writes a bare-bones PDF with sc-pdflite, simply-cpp's own writer: text in PDF's built-in fonts
// (measured with sc::standard_font, so a line can be centred), boxes, a curve and a clipped
// drawing, on two pages. Positions are from the top-left corner, y down, as on sc-graphics' canvas. Each line shows a call and what it returned.

#include <filesystem>
#include <iostream>
#include <string>

#include <params.h>
#include <sc.h>

#include <distance.h>
#include <pdflite.h>
#include <standard_font.h>

int main(int argc, char **argv) {
    const sc::params args{argc, argv, {
        {"name", "sc-pdflite-demo"},
        {"description", "Writes two pages with sc-pdflite, simply-cpp's own PDF writer: text in the built-in fonts, "
                        "shapes and clipping, in points from the top-left corner (y down), as on sc-graphics' canvas."},
        {"positional", {
            {"output", {{"default", (std::filesystem::temp_directory_path() / "sc-pdflite-demo.pdf").string()},
                        {"help", "the PDF to write"}}}}}}};

    try {
        const std::filesystem::path output = args.as<std::string>("output");
        sc::console::title("simply-cpp pdflite: our own PDF writer");
        sc::timer sw;

        // [readme]
        sc::console::heading("A document of A4 pages: sc-graphics' paper sizes, or any size in points");
        sc::pdflite::document pdf{output, sc::page_size::A4};
        sc::console::step("sc::pdflite::document pdf{output, sc::page_size::A4};   // " + output.string());
        SC_SHOW(pdf.width());  // a distance, in points (1/72 inch)
        SC_SHOW(pdf.height());
        SC_SHOW(static_cast<sc::dist::mm>(pdf.width())); // the same, in millimetres

        sc::console::heading("Text: a built-in font, then BT ... ET");
        auto &page = pdf.page_content();
        const auto bold = pdf.standard_font(sc::standard_font::face::helvetica_bold);
        SC_SHOW(bold); // its name in the page's resources
        const std::string heading = "simply-cpp pdflite";
        const sc::standard_font metrics{sc::standard_font::face::helvetica_bold}; // Adobe's metrics for it
        const sc::distance width = metrics.width(heading, 28);
        SC_SHOW(width); // Adobe's widths: lines are measured as readers draw them
        page.begin_text();
        page.font(bold, 28);
        page.text_position((pdf.width() - width) / 2, 82); // centred, the baseline 82 points from the top
        page.show(*sc::pdflite::document::win_ansi(heading));
        page.end_text();

        const auto serif = pdf.standard_font(sc::standard_font::face::times_roman);
        page.begin_text();
        page.font(serif, 13);
        page.text_position(sc::dist::inch(1), 122); // a unit from sc::dist, or a plain number of points
        SC_STEP(page.show(*sc::pdflite::document::win_ansi("Strict PDF 1.7, no library but zlib: caf\xC3\xA9, \xE2\x82\xAC 12, (brackets) and \\.")));
        page.end_text();

        sc::console::heading("Shapes: a path, then how to paint it");
        page.save();
        page.fill_colour(0.85, 0.93, 0.98);
        page.stroke_colour(0.16, 0.44, 0.59);
        page.line_width(2);
        SC_STEP(page.rectangle(sc::dist::mm(25.4), 162, sc::dist::mm(77.6), 120)); // left, top, width, height
        SC_STEP(page.fill_and_stroke());
        page.fill_colour(0.93, 0.42, 0.3);
        page.move(320, 282);
        page.curve(360, 142, 480, 142, 520, 282);
        page.close_path();
        SC_STEP(page.fill());
        page.restore();

        sc::console::heading("Clipping: W n, then everything drawn is cut to it");
        page.save();
        page.rectangle(72, 322, 200, 140);
        page.clip();
        page.end_path();
        page.fill_colour(0.96, 0.83, 0.37);
        for (int i = 0; i < 8; ++i) {
            page.move(72 + i * 40, 462);
            page.line(32 + i * 40, 322);
            page.line(52 + i * 40, 322);
            page.line(92 + i * 40, 462);
            page.close_path();
            page.fill();
        }
        page.restore();
        SC_SHOW(page.complete()); // q balanced, no open text or path: ready to close

        sc::console::heading("A second page, and the document's information");
        SC_STEP(pdf.add_page());
        auto &second = pdf.page_content();
        const auto mono = pdf.standard_font(sc::standard_font::face::courier);
        second.begin_text();
        second.font(mono, 11);
        second.text_position(72, 82);
        second.show("Page two, in Courier.");
        second.end_text();
        SC_STEP(pdf.info("Title", "sc-pdflite demo"));
        SC_STEP(pdf.close());
        SC_SHOW(std::filesystem::file_size(output));
        // [/readme]

        sc::console::output() << "\nThe result is " << output.string() << "\nAll of that took " << sw << ".\n";
    } catch (const std::exception &error) {
        std::cerr << "sc-pdflite-demo: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
