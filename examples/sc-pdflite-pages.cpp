// Writes a PDF whose pages differ in size: page 1 an A4 portrait page, page 2 an A3 landscape one.
// Each page gets a frame 10 mm in from its edges and its size, centred, so the difference shows; a
// link and bookmarks go from one to the other. Each line shows a call and what it returned.

#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>

#include <params.h>
#include <sc.h>

#include <distance.h>
#include <pdflite.h>
#include <standard_font.h>

namespace {
    // A frame 10 mm in from the current page's edges, and its size in the middle.
    void frame_and_size(sc::pdflite::document &pdf, const std::string &paper) {
        auto &page = pdf.page_content();
        const sc::dist::mm margin{10};
        page.save();
        page.stroke_colour(0.16, 0.44, 0.59);
        page.line_width(sc::dist::mm(0.5));
        page.rectangle(margin, margin, pdf.size().width() - 2 * margin, pdf.size().height() - 2 * margin);
        page.stroke();
        page.restore();

        std::ostringstream size;
        const auto in_mm = static_cast<sc::size_<sc::dist::mm>>(pdf.size());
        size << paper << ": " << in_mm.width() << " x " << in_mm.height();
        const sc::standard_font font{sc::standard_font::face::helvetica_bold};
        const double text_size = 28;
        page.begin_text();
        page.font(pdf.standard_font(font.which()), text_size);
        page.text_position((pdf.size().width() - font.width(size.str(), text_size)) / 2, pdf.size().height() / 2); // centred
        page.show(*sc::pdflite::document::win_ansi(size.str()));
        page.end_text();
    }
}

int main(int argc, char **argv) {
    const sc::params args{argc, argv, {
        {"name", "sc-pdflite-pages"},
        {"description", "Writes a PDF with pages of different sizes, an A4 portrait page and an A3 landscape one, each "
                        "framed with its size written in the middle, and a link from the first to the second."},
        {"positional", {
            {"output", {{"default", (std::filesystem::temp_directory_path() / "sc-pdflite-pages.pdf").string()},
                        {"help", "the PDF to write"}}}}}}};

    try {
        const std::filesystem::path output = args.as<std::string>("output");
        sc::console::title("simply-cpp pdflite: pages of different sizes");
        sc::timer sw;

        // [readme]
        sc::console::heading("Page 1: the document's size, A4 portrait");
        sc::pdflite::document pdf{output, sc::page_size::A4};
        SC_SHOW(static_cast<sc::size_<sc::dist::mm>>(pdf.size())); // the current page's size, in mm
        frame_and_size(pdf, "A4 portrait");

        sc::console::heading("Page 2: a size of its own, A3 landscape (add_page(paper, orientation))");
        SC_STEP(pdf.add_page(sc::page_size::A3, sc::orientation::landscape));
        SC_SHOW(static_cast<sc::size_<sc::dist::mm>>(pdf.size())); // now page 2's
        frame_and_size(pdf, "A3 landscape");

        sc::console::heading("Links and bookmarks find each page's own size");
        pdf.set_page(1);
        const auto font = pdf.standard_font(sc::standard_font::face::helvetica);
        auto &first = pdf.page_content();
        first.begin_text();
        first.font(font, 12);
        first.text_position(sc::dist::mm(25), sc::dist::mm(30));
        first.show("To the A3 page");
        first.end_text();
        SC_STEP(pdf.link(sc::rect{sc::dist::mm(25), sc::dist::mm(25), sc::dist::mm(32), sc::dist::mm(7)}, 2));
        SC_STEP(pdf.bookmark("A4 portrait", 1));
        SC_STEP(pdf.bookmark("A3 landscape", 2));
        pdf.info("Title", "sc-pdflite: pages of different sizes");
        SC_STEP(pdf.close());
        SC_SHOW(std::filesystem::file_size(output)); // bytes
        // [/readme]

        sc::console::output() << "\nThe result is " << output.string() << "\nAll of that took " << sw << ".\n";
    } catch (const std::exception &error) {
        std::cerr << "sc-pdflite-pages: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
