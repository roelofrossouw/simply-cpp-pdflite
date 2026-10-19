// Writes a small booklet with sc-pdflite: a contents page whose lines go to the chapters, the
// chapters on pages of their own with a way back, a web link, and bookmarks for the reader's
// outline, chapters with their sections under them. Each line shows a call and what it returned.

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <params.h>
#include <sc.h>

#include <pdflite.h>
#include <standard_font.h>

namespace {
    // A line of text in a built-in font, its baseline starting at (x, y), and the area it covers,
    // for a link.
    sc::rect write_line(sc::pdflite::document &pdf, const sc::standard_font::face font, const double size,
                        const double x, const double y, const std::string &text) {
        auto &page = pdf.page_content();
        page.begin_text();
        page.font(pdf.standard_font(font), size);
        page.text_position(x, y);
        page.show(*sc::pdflite::document::win_ansi(text));
        page.end_text();
        const sc::standard_font metrics{font};
        return {x, y - metrics.ascender(size), metrics.width(text, size), metrics.ascender(size) + metrics.descender(size)};
    }
}

int main(int argc, char **argv) {
    const sc::params args{argc, argv, {
        {"name", "sc-pdflite-links"},
        {"description", "Writes a booklet with sc-pdflite: a contents page that links to its chapters, a link back on "
                        "every chapter, a web link, and bookmarks (chapters with their sections) for the reader's outline."},
        {"positional", {
            {"output", {{"default", (std::filesystem::temp_directory_path() / "sc-pdflite-links.pdf").string()},
                        {"help", "the PDF to write"}}}}}}};

    try {
        const std::filesystem::path output = args.as<std::string>("output");
        sc::console::title("simply-cpp pdflite: links and bookmarks");
        sc::timer sw;

        // [readme]
        sc::pdflite::document pdf{output, sc::page_size::A4};
        const double top = 72; // the headings' baselines, an inch from the top
        const std::vector<std::string> chapters{"Getting started", "Drawing", "Text and fonts"};

        sc::console::heading("Pages first: a link may go to a page that comes later, but it must be there by close()");
        for (std::size_t i = 0; i < chapters.size(); ++i) pdf.add_page(); // pages 2 to 4
        SC_SHOW(pdf.page_count());

        sc::console::heading("The contents page: each line a link to its chapter (link(area, page, top))");
        pdf.set_page(1);
        write_line(pdf, sc::standard_font::face::helvetica_bold, 24, 72, top, "Contents");
        for (std::size_t i = 0; i < chapters.size(); ++i) {
            const auto line = write_line(pdf, sc::standard_font::face::times_roman, 14, 90, top + 50 + 24.0 * i, std::to_string(i + 1) + ". " + chapters[i]);
            SC_STEP(pdf.link(line, i + 2, top - 20)); // the chapter's page, from just above its heading
        }

        sc::console::heading("A web link (link(area, url)): the address is written in ASCII, percent-encoded");
        const auto site = write_line(pdf, sc::standard_font::face::helvetica, 11, 72, pdf.size().height() - 100, "simply-cpp on GitHub");
        SC_STEP(pdf.link(site, "https://github.com/roelofrossouw/simply-cpp"));

        sc::console::heading("Chapters, each with a way back to the contents");
        for (std::size_t i = 0; i < chapters.size(); ++i) {
            pdf.set_page(i + 2);
            write_line(pdf, sc::standard_font::face::helvetica_bold, 20, 72, top, std::to_string(i + 1) + ". " + chapters[i]);
            write_line(pdf, sc::standard_font::face::times_roman, 12, 72, top + 40, "Section " + std::to_string(i + 1) + ".1");
            write_line(pdf, sc::standard_font::face::times_roman, 12, 72, top + 300, "Section " + std::to_string(i + 1) + ".2");
            const auto back = write_line(pdf, sc::standard_font::face::helvetica, 10, 72, pdf.size().height() - 60, "Back to the contents");
            pdf.link(back, 1); // the top of the contents page
        }

        sc::console::heading("Bookmarks: level 0 for chapters, 1 for their sections");
        SC_STEP(pdf.bookmark("Contents", 1)); // the top of page 1
        for (std::size_t i = 0; i < chapters.size(); ++i) {
            SC_STEP(pdf.bookmark(chapters[i], i + 2, top - 20));
            pdf.bookmark("Section " + std::to_string(i + 1) + ".1", i + 2, top + 25, 1);
            pdf.bookmark("Section " + std::to_string(i + 1) + ".2", i + 2, top + 285, 1);
        }
        pdf.info("Title", "sc-pdflite: links and bookmarks");
        pdf.info("Author", "Roelof Rossouw");
        pdf.info("Subject", "A contents page linking to its chapters, links back, a web link and nested bookmarks");
        pdf.info("Creator", "sc-pdflite-links");
        SC_STEP(pdf.close());
        SC_SHOW(std::filesystem::file_size(output)); // bytes
        // [/readme]

        sc::console::output() << "\nThe result is " << output.string() << "\nAll of that took " << sw << ".\n";
    } catch (const std::exception &error) {
        std::cerr << "sc-pdflite-links: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
