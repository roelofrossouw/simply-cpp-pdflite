// Writes a PDF with sc::pdflite::writer: text, shapes, SVG drawings, a half-transparent circle over the
// boxes, a link, and a race track placed in the bottom-right corner by its bounds(). Each line shows
// a call and what it returned. sc-mupdf-write draws the same page with MuPDF.

#include <filesystem>
#include <iostream>
#include <string>
#include <utility>

#include <sc/core/params.h>
#include <sc/core/sc.h>

#include <sc/pdflite/writer.h>

int main(int argc, char **argv) {
    // sc::params brings -h/--help, and reports mistakes and exits.
    const sc::params args{argc, argv, {
        {"name", "sc-pdflite-writer"},
        {"description", "Writes a page of text, shapes, SVG drawings and a link with sc::pdflite::writer, to show what sc::pdflite::writer can "
                        "draw. sc-mupdf-write writes the same page with MuPDF, if you'd like to compare the two."
                        "\nMore on the wiki: https://github.com/roelofrossouw/simply-cpp/wiki/PDF"},
        {"options", {
            {"size", {{"default", "A4"}, {"choices", {"A3", "A4", "A5", "letter", "legal"}}, {"help", "the paper"}}},
            {"landscape", {{"type", "boolean"}, {"help", "turn the pages sideways"}}},
            {"svg", {{"value", "FILE"}, {"help", "an SVG file to draw on a page of its own"}}}}},
        {"positional", {
            {"output", {{"default", (std::filesystem::temp_directory_path() / "sc-pdflite-writer.pdf").string()},
                        {"help", "the PDF to write"}}}}}}};

    try {
        const std::filesystem::path output = args.as<std::string>("output");
        const std::filesystem::path svg_file = args.as<std::string>("svg", "");
        const auto paper = sc::graphics::page_size_from(args.as<std::string>("size"));
        const auto turned = args.as<bool>("landscape") ? sc::graphics::orientation::landscape : sc::graphics::orientation::portrait;
        sc::console::title("simply-cpp pdf: text, shapes, SVG drawings, a link and a corner path");
        sc::timer sw;

        // [readme]
        sc::console::heading("Starting the file");
        sc::pdflite::writer pdf{output, paper, turned};
        sc::console::step("sc::pdflite::writer pdf{output, paper, turned};   // " + output.string());
        SC_SHOW(pdf.page_width());  // points, 1/72 inch: --size and --landscape
        SC_SHOW(pdf.page_height());

        sc::console::heading("Text: sc::graphics::text{content, where its baseline starts, sc::graphics::font}");
        SC_STEP(pdf.draw(sc::graphics::text{"simply-cpp pdf", {50, 90}, sc::graphics::font::bold("sans-serif", 32)}));
        SC_STEP(pdf.draw(sc::graphics::text{"Text, shapes and SVG on sc-pdflite", {50, 120}, {"serif", 14, 400, true}}));
        sc::graphics::text centred{"This line is centred on the page", {0, 150}}; // the default font, sans-serif 12
        SC_SHOW(pdf.text_width(centred));
        centred.position.x((pdf.page_width() - pdf.text_width(centred)) / 2);
        SC_STEP(pdf.draw(centred, sc::color{0.2, 0.2, 0.6}));

        sc::console::heading("Lines and shapes: draw(geometry, appearance)");
        SC_STEP(pdf.draw(sc::line{{50, 165}, {pdf.page_width() - 50, 165}}, sc::graphics::stroke{sc::color::Black, 0.5}));
        const sc::rect box{50, 190, 230, 120};
        SC_STEP(pdf.draw(sc::rounded_rect{box, 12}, {sc::graphics::stroke{sc::color{0.16, 0.44, 0.59}, 2}, sc::color{0.85, 0.93, 0.98}})); // stroke, fill
        SC_STEP(pdf.draw(sc::graphics::text{"A rounded box", {70, 255}, {"sans-serif", 16}}));
        SC_STEP(pdf.draw(sc::circle{{110, 400}, 40}, {sc::graphics::stroke{sc::color::Black, 2}, sc::color{0.96, 0.83, 0.37}}));
        SC_STEP(pdf.draw(sc::polygon{{180, 440}, {230, 360}, {280, 440}}, sc::graphics::stroke{sc::color{0.93, 0.42, 0.3}, 3}));
        SC_STEP(pdf.draw(sc::rotated_rect{{380, 400}, {120, 50}, 20}, {sc::color::Black, sc::color{0.85, 0.93, 0.98}}));

        sc::console::heading("An SVG drawing, scaled to fit and centred");
        const std::string svg = R"(<svg xmlns="http://www.w3.org/2000/svg" width="320" height="200">)"
                                R"(<rect width="320" height="200" rx="16" fill="#2a6f97"/>)"
                                R"(<circle cx="160" cy="100" r="70" fill="#f4d35e"/>)"
                                R"(<path d="M120 130 Q160 170 200 130" stroke="#2a6f97" stroke-width="8" fill="none"/></svg>)";
        SC_STEP(pdf.draw(sc::graphics::drawing::from_svg(svg), sc::rect{315, 190, 230, 120}));

        sc::console::heading("Transparency: a colour's alpha");
        // Over both boxes: filled half opaque, outlined three-quarters.
        SC_STEP(pdf.draw(sc::circle{{297.5, 250}, 75}, {sc::graphics::stroke{sc::color{0.75, 0.1, 0.1, 0.75}, 3}, sc::color{0.9, 0.2, 0.2, 0.5}}));

        sc::console::heading("A link");
        SC_STEP(pdf.draw(sc::graphics::text{"github.com/roelofrossouw/simply-cpp", {50, 490}}, sc::color::Blue));
        SC_STEP(pdf.add_link("https://github.com/roelofrossouw/simply-cpp", {50, 476, 220, 18}));

        sc::console::heading("More SVG: transforms, arcs with an even-odd hole, a clip path");
        const auto star = sc::graphics::drawing::from_svg(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">)svg"
                                                R"svg(<g transform="rotate(15 50 50)"><polygon fill="#e76f51" stroke="#264653" stroke-width="3")svg"
                                                R"svg( points="50,5 61,38 95,38 67,58 78,92 50,72 22,92 33,58 5,38 39,38"/></g></svg>)svg");
        const auto ring = sc::graphics::drawing::from_svg(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">)svg"
                                                R"svg(<path fill="#2a9d8f" fill-rule="evenodd" d="M5 50 A45 45 0 1 1 95 50 A45 45 0 1 1 5 50 Z)svg"
                                                R"svg( M25 50 A25 25 0 1 0 75 50 A25 25 0 1 0 25 50 Z"/></svg>)svg");
        const auto disc = sc::graphics::drawing::from_svg(R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">)svg"
                                                R"svg(<defs><clipPath id="round"><circle cx="50" cy="50" r="45"/></clipPath></defs>)svg"
                                                R"svg(<rect clip-path="url(#round)" width="100" height="50" fill="#e9c46a"/>)svg"
                                                R"svg(<rect clip-path="url(#round)" y="50" width="100" height="50" fill="#264653"/></svg>)svg");
        double top = 530, left_edge = 50;
        if (top + 300 > pdf.page_height()) { // no room on a shorter page (Letter, landscape): the frames go on the next
            SC_STEP(pdf.new_page());
            top = 50;
        }
        for (const auto &[drawing, caption]: {std::pair{&star, "rotate(15)"}, std::pair{&ring, "evenodd"}, std::pair{&disc, "clip-path"}}) {
            const sc::rect frame{left_edge, top, 150, 150};
            pdf.draw(frame, sc::graphics::stroke{sc::color{0.8, 0.8, 0.8}, 0.5});
            pdf.draw(*drawing, frame - 10); // 10 points inside the frame
            pdf.draw(sc::graphics::text{caption, {left_edge, top + 168}, {"monospace", 10}});
            left_edge += 172.5;
        }
        sc::console::step("pdf.draw(star / ring / disc, frame - 10);   // three 150-point frames");

        sc::console::heading("A path in the bottom-right corner, placed by its bounds()");
        // The Nurburgring's Nordschleife, roughly: one smooth curve through its corners, from
        // Hatzenbach up to the Flugplatz, out to Brunnchen and back down the Dottinger Hohe. Where
        // its curves bend furthest isn't at any of their points, so bounds() finds the extremes.
        sc::path track;
        track.move_to({20, 70})
            .cubic_to({20.8, 62.8}, {24.2, 51.7}, {30, 45})     // Hatzenbach
            .cubic_to({35.8, 38.3}, {48.3, 35.8}, {55, 30})
            .cubic_to({61.7, 24.2}, {63.3, 12.5}, {70, 10})     // Flugplatz
            .cubic_to({76.7, 7.5}, {88.3, 10}, {95, 15})        // Adenauer Forst
            .cubic_to({101.7, 20}, {105, 32.5}, {110, 40})
            .cubic_to({115, 47.5}, {119.2, 59.2}, {125, 60})    // Bergwerk
            .cubic_to({130.8, 60.8}, {137.5, 49.2}, {145, 45})  // Karussell
            .cubic_to({152.5, 40.8}, {162.5, 34.2}, {170, 35})  // Hohe Acht
            .cubic_to({177.5, 35.8}, {187.5, 43.3}, {190, 50})  // Brunnchen
            .cubic_to({192.5, 56.7}, {189.2, 69.2}, {185, 75})  // Pflanzgarten
            .cubic_to({180.8, 80.8}, {177.5, 81.7}, {165, 85})
            .cubic_to({152.5, 88.3}, {129.2, 93.3}, {110, 95})  // Dottinger Hohe
            .cubic_to({90.8, 96.7}, {64.2, 96.2}, {50, 95})
            .cubic_to({35.8, 93.8}, {30, 92.2}, {25, 88})       // Tiergarten
            .cubic_to({20, 83.8}, {19.2, 77.2}, {20, 70})
            .close();
        SC_SHOW(track.bounds());
        const sc::point corner{pdf.page_width() - 8, pdf.page_height() - 8};
        track = sc::transform::translate(corner - track.bounds().right_bottom()).apply(track);
        SC_STEP(pdf.draw(track.bounds(), sc::graphics::stroke{sc::color{0.6, 0.6, 0.6}, 0.5})); // its bounds: the line's centre touches it all round
        SC_STEP(pdf.draw(track, sc::graphics::stroke{sc::color::Black, 4}));

        sc::console::heading("Finishing the file");
        if (!svg_file.empty()) {
            SC_STEP(pdf.new_page());
            const auto drawing = sc::graphics::drawing::load_svg(svg_file);
            sc::console::step("const auto drawing = sc::graphics::drawing::load_svg(svg_file);   // " + svg_file.string());
            SC_SHOW(drawing.size());
            SC_STEP(pdf.draw(drawing, sc::rect{50, 50, pdf.page_width() - 100, pdf.page_height() - 100}));
        }
        SC_SHOW(pdf.page_count());
        SC_STEP(pdf.close());
        SC_SHOW(std::filesystem::file_size(output)); // bytes
        // [/readme]

        sc::console::output() << "\nThe result is " << output.string() << "\nAll of that took " << sw << ".\n";
    } catch (const std::exception &error) {
        std::cerr << "sc-pdflite-writer: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
