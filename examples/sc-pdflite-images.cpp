// Writes a page of JPEG images with sc-pdflite: a photo of a cat as it is, mirrored, turned and
// scaled (each an sc::transform of the image's unit square), and a picture drawn with sc-image
// and encoded as a JPEG in memory. The photo is written into the file once, however often it's
// drawn. Each line shows a call and what it returned.
//
// The photo, examples/resource/cat.jpg, is "Cat is sitting on the wall" by Vyshnavi medicharla
// (Wikimedia Commons, CC0 1.0, public domain): https://commons.wikimedia.org/wiki/File:Cat_is_sitting_on_the_wall.jpg

#include <filesystem>
#include <iostream>
#include <string>

#include <params.h>
#include <sc.h>

#include <distance.h>
#include <opencv.h>
#include <pdflite.h>
#include <standard_font.h>

// The units by their plain names: mm(10), inch(1), pdf.size().in<mm>().
using namespace sc::dist;

namespace {
    // A caption in Helvetica, centred under box.
    void caption(sc::pdflite::document &pdf, const sc::rect &box, const std::string &text) {
        const sc::standard_font font{sc::standard_font::face::helvetica};
        auto &page = pdf.page_content();
        page.begin_text();
        page.font(pdf.standard_font(font.which()), 10);
        page.text_position(box.left() + (box.width() - font.width(text, 10)) / 2, box.bottom() + 14);
        page.show(text);
        page.end_text();
    }

    // The placement that fills box with the image: its unit square scaled to the box and moved there.
    sc::transform filling(const sc::rect &box) { return {box.width(), 0, 0, box.height(), box.left(), box.top()}; }
}

int main(int argc, char **argv) {
    const sc::params args{argc, argv, {
        {"name", "sc-pdflite-images"},
        {"description", "Writes a page of JPEG images with sc-pdflite: a photo as it is, mirrored, turned and scaled, "
                        "and a picture drawn with sc-image, encoded as a JPEG in memory."},
        {"options", {
            {"photo", {{"value", "FILE"}, {"default", SC_PDFLITE_EXAMPLE_PHOTO}, {"help", "the JPEG photo to draw"}}}}},
        {"positional", {
            {"output", {{"default", (std::filesystem::temp_directory_path() / "sc-pdflite-images.pdf").string()},
                        {"help", "the PDF to write"}}}}}}};

    try {
        const std::filesystem::path output = args.as<std::string>("output");
        sc::console::title("simply-cpp pdflite: JPEG images, transformed");
        sc::timer sw;

        // [readme]
        sc::pdflite::document pdf{output, sc::page_size::A4};

        sc::console::heading("A JPEG, written as it is: its header is read for the size and colours");
        const auto photo = sc::pdflite::jpeg::load(args.as<std::string>("photo"));
        SC_SHOW(photo.size()); // in pixels
        SC_SHOW(photo.components()); // 3: RGB
        const auto cat = pdf.image(photo);
        SC_SHOW(cat); // its name in the page's resources

        sc::console::heading("Drawn into a box, keeping its proportions: draw(name, rect)");
        const sc::distance width = mm(80);
        const sc::distance height = width * photo.size().height() / photo.size().width();
        const sc::rect original{mm(20), mm(25), width, height};
        auto &page = pdf.page_content();
        SC_STEP(page.draw(cat, original));
        caption(pdf, original, "As it is");

        sc::console::heading("Through an sc::transform of its unit square: mirrored, turned, scaled");
        const sc::rect mirrored_box{mm(110), mm(25), width, height};
        SC_STEP(page.draw(cat, filling(mirrored_box).then(sc::transform::flip_horizontal(mirrored_box.center().x()))));
        caption(pdf, mirrored_box, "Mirrored: flip_horizontal");

        const sc::rect turned_box{mm(20), mm(105), width, height};
        const auto turn = sc::transform::rotate(30, turned_box.center());
        SC_STEP(page.draw(cat, filling(turned_box).then(turn)));
        caption(pdf, sc::to_path(turn.apply(turned_box)).bounds(), "Turned 30 degrees about its centre: rotate"); // under where it went

        const sc::rect scaled_box{mm(110), mm(105), width, height};
        SC_STEP(page.draw(cat, filling(scaled_box).then(sc::transform::scale(0.5, scaled_box.center()))));
        caption(pdf, scaled_box, "Half the size, about its centre: scale");

        sc::console::heading("A picture drawn with sc-image, encoded as a JPEG in memory");
        sc::image drawn{{480, 320}, sc::color{0.85, 0.93, 0.98}};
        drawn.draw(sc::circle{{160, 160}, 110}, {sc::stroke{sc::color{0.16, 0.44, 0.59}, 8}, sc::color{0.96, 0.83, 0.37}});
        drawn.draw(sc::rect{250, 70, 180, 180}, sc::appearance::filled(sc::color{0.93, 0.42, 0.3}));
        drawn.draw(sc::text{"sc-image", {40, 300}, sc::font::bold("sans-serif", 28)}, sc::color{0.1, 0.1, 0.1});
        const auto picture = sc::pdflite::jpeg(drawn.encoded("jpg", 90));
        SC_SHOW(picture.size());
        const sc::rect picture_box{mm(55), mm(190), mm(100), mm(100) * 320 / 480};
        SC_STEP(page.draw(pdf.image(picture), picture_box));
        caption(pdf, picture_box, "Drawn with sc-image, encoded as a JPEG");

        pdf.info("Title", "sc-pdflite: JPEG images");
        pdf.info("Author", "Roelof Rossouw");
        pdf.info("Subject", "A JPEG photo as it is, mirrored, turned and scaled, and a picture drawn with sc-image");
        pdf.info("Creator", "sc-pdflite-images");
        SC_STEP(pdf.close());
        SC_SHOW(std::filesystem::file_size(output)); // the photo is in it once, though drawn four times
        // [/readme]

        sc::console::output() << "\nThe result is " << output.string() << "\nAll of that took " << sw << ".\n";
    } catch (const std::exception &error) {
        std::cerr << "sc-pdflite-images: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
