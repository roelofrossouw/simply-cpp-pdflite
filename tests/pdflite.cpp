// sc-pdflite: how values are spelt, the content stream's rules, and files
// whose structure is checked byte by byte here; ctest's check-* tests then run qpdf, pdfcpu and
// mutool over every file written to SC_PDFLITE_OUTPUT.
#include "pdflite.h"

#include "sc_test.h"

#include <base64.h>
#include <console.h>

#include <zlib.h>

#include <font.h>
#include <font_face.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <array>
#include <regex>
#include <stdexcept>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace sc::pdflite;

namespace {
    fs::path output() {
        const char *where = std::getenv("SC_PDFLITE_OUTPUT");
        fs::path out = where && *where ? fs::path{where} : fs::temp_directory_path() / "sc-pdflite";
        fs::create_directories(out);
        return out;
    }

    std::string read(const fs::path &path) {
        std::ifstream in{path, std::ios::binary};
        return {std::istreambuf_iterator<char>{in}, {}};
    }

    bool contains(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }

    // Whether the file's structure is exact: the header, a cross-reference table whose every entry
    // is 20 bytes and points at its own object, /Size counting them, and startxref at the table.
    std::string structure_problem(const std::string &file) {
        if (!file.starts_with("%PDF-1.7\n%")) return "no PDF 1.7 header";
        if (!file.ends_with("%%EOF\n")) return "no %%EOF at the end";
        const auto start_at = file.rfind("startxref\n");
        if (start_at == std::string::npos) return "no startxref";
        const auto table = std::stoull(file.substr(start_at + 10));
        if (file.compare(table, 5, "xref\n") != 0) return "startxref doesn't point at the table";
        std::size_t at = table + 5;
        const auto line_end = file.find('\n', at);
        const auto header = file.substr(at, line_end - at); // "0 N"
        if (!header.starts_with("0 ")) return "the table doesn't start at object 0";
        const auto count = std::stoull(header.substr(2));
        at = line_end + 1;
        if (file.compare(at, 20, "0000000000 65535 f \n") != 0) return "object 0 isn't the free list's head";
        for (std::size_t id = 1; id < count; ++id) {
            const auto entry = file.substr(at + id * 20, 20);
            if (entry.size() != 20 || entry.compare(10, 10, " 00000 n \n") != 0) return "entry " + std::to_string(id) + " isn't 20 bytes";
            const auto offset = std::stoull(entry.substr(0, 10));
            if (file.compare(offset, std::to_string(id).size() + 7, std::to_string(id) + " 0 obj\n") != 0)
                return "entry " + std::to_string(id) + " doesn't point at its object";
        }
        if (file.compare(at + count * 20, 8, "trailer\n") != 0) return "no trailer after the table";
        if (!contains(file, "/Size " + std::to_string(count) + " ")) return "/Size isn't the table's length";
        return {};
    }

    // Every FlateDecode stream's bytes, inflated, one after another.
    std::string inflated_streams(const std::string &file) {
        std::string out;
        static const std::regex stream_head{R"(/Length (\d+) /Filter /FlateDecode >>\nstream\n)"};
        for (std::sregex_iterator it{file.begin(), file.end(), stream_head}, end; it != end; ++it) {
            const auto length = std::stoul((*it)[1]);
            const auto data = file.substr(it->position() + it->length(), length);
            std::string plain(1 << 20, '\0');
            uLongf size = plain.size();
            if (uncompress(reinterpret_cast<Bytef *>(plain.data()), &size, reinterpret_cast<const Bytef *>(data.data()), data.size()) != Z_OK)
                return "(a stream that doesn't inflate)";
            out += plain.substr(0, size);
        }
        return out;
    }
}

int main() {
    SECTION("Numbers: integers when whole, four decimals (six significant digits below 1), no exponent");
    CHECK_EQ(number(0), std::string{"0"});
    CHECK_EQ(number(-0.00000000001), std::string{"0"}); // never -0
    CHECK_EQ(number(1 / 595.2756), std::string{"0.00167989"}); // six significant digits below 1
    CHECK_EQ(number(0.5), std::string{"0.5"});
    CHECK_EQ(number(595.2756), std::string{"595.2756"});
    CHECK_EQ(number(12.345678), std::string{"12.3457"});
    CHECK_EQ(number(-1.5), std::string{"-1.5"});
    CHECK_EQ(number(842), std::string{"842"});
    CHECK_EQ(number(1e10), std::string{"10000000000.0"}); // beyond 32 bits: written as a real
    CHECK_THROWS_AS(number(std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
    CHECK_THROWS_AS(number(1e39), std::invalid_argument);

    SECTION("Names, strings and text strings");
    CHECK_EQ(name("Helvetica-Bold").write(), std::string{"/Helvetica-Bold"});
    CHECK_EQ(name("A B/C#").write(), std::string{"/A#20B#2FC#23"});
    CHECK_THROWS_AS(name(""), std::invalid_argument);
    CHECK_THROWS_AS(name(std::string_view{"a\0b", 3}), std::invalid_argument);
    CHECK_EQ(string("a(b)\\").write(), std::string{"(a\\(b\\)\\\\)"});
    CHECK_EQ(string("x\n\x80").write(), std::string{"(x\\012\\200)"});
    CHECK_EQ(string("\x01\xAB", true).write(), std::string{"<01AB>"});
    CHECK_EQ(string::text("Plain title").write(), std::string{"(Plain title)"});
    CHECK_EQ(string::text("Caf\xC3\xA9").write(), std::string{"<FEFF00430061006600E9>"});
    CHECK_EQ(string::text("\xF0\x9F\x98\x80").write(), std::string{"<FEFFD83DDE00>"}); // 😀: a surrogate pair
    CHECK_THROWS_AS(string::text("\xC3"), std::invalid_argument);
    {
        const auto stamp = string::date(std::chrono::system_clock::now()).write();
        CHECK(std::regex_match(stamp, std::regex{R"(\(D:\d{14}(Z|[+-]\d\d'\d\d')\))"}));
    }

    SECTION("Objects: every kind writes itself, and is checked as it's made");
    CHECK_EQ(object{}.write(), std::string{"null"});
    CHECK_EQ(object{true}.write(), std::string{"true"});
    CHECK_EQ(object{-42}.write(), std::string{"-42"});
    CHECK_EQ(object{std::size_t{7}}.write(), std::string{"7"});
    CHECK_EQ(object{2.5}.write(), std::string{"2.5"});
    CHECK_EQ(object{reference{12}}.write(), std::string{"12 0 R"});
    CHECK_THROWS_AS(object{reference{0}}.write(), std::invalid_argument);
    CHECK_THROWS_AS(object{5'000'000'000LL}, std::invalid_argument); // beyond 32 bits
    CHECK_THROWS_AS(object{std::numeric_limits<double>::infinity()}, std::invalid_argument);
    CHECK_EQ(array{}.write(), std::string{"[]"});
    CHECK_EQ(dictionary{}.write(), std::string{"<< >>"});
    {
        const dictionary page{{"Type", name{"Page"}}, {"MediaBox", array{0, 0, 612, 792}}, {"Rotate", 0},
                              {"Info", dictionary{{"Title", string::text("A")}, {"Nothing", nullptr}}}};
        CHECK_EQ(page.write(), std::string{"<< /Type /Page /MediaBox [0 0 612 792] /Rotate 0 /Info << /Title (A) /Nothing null >> >>"});
        CHECK_EQ(page.size(), std::size_t{4});
        CHECK(page.at("Type").is<name>());
        CHECK_EQ(page.at("Type").as<name>().text(), std::string{"Page"});
        CHECK_EQ(page.at("MediaBox").as<array>().size(), std::size_t{4});
        CHECK(page.find("Missing") == nullptr);
        CHECK_THROWS_AS(page.at("Missing"), std::out_of_range);
        CHECK_THROWS_AS((dictionary{{"Type", name{"A"}}, {"Type", name{"B"}}}), std::logic_error); // a key twice
        dictionary changed = page;
        changed.set("Rotate", 90).set("Extra", false);
        CHECK_EQ(changed.at("Rotate").as<std::int64_t>(), std::int64_t{90});
        CHECK_EQ(changed.size(), std::size_t{5});
        CHECK_THROWS_AS(changed.add("Extra", true), std::logic_error);
        CHECK_THROWS_AS(changed.add("", true), std::invalid_argument); // a key is a name
    }

    SECTION("WinAnsiEncoding for the built-in fonts");
    CHECK_EQ(document::win_ansi("Caf\xC3\xA9 \xE2\x82\xAC").value_or("?"), std::string{"Caf\xE9 \x80"}); // é, €
    CHECK(!document::win_ansi("\xD0\x96")); // Ж isn't in it

    SECTION("The content stream keeps PDF's rules");
    {
        content c{842};
        CHECK_THROWS_AS(c.restore(), std::logic_error);        // Q without q
        CHECK_THROWS_AS(c.line(1, 1), std::logic_error);       // l without m
        CHECK_THROWS_AS(c.fill(), std::logic_error);           // nothing to fill
        CHECK_THROWS_AS(c.end_text(), std::logic_error);       // ET without BT
        c.begin_text();
        CHECK_THROWS_AS(c.show("x"), std::logic_error);        // Tj before Tf
        CHECK_THROWS_AS(c.move(0, 0), std::logic_error);       // a path in a text object
        CHECK_THROWS_AS(c.save(), std::logic_error);           // q in a text object
        CHECK_THROWS_AS(c.begin_text(), std::logic_error);     // BT in BT
        c.font("F1", 12);
        c.show("x");
        c.end_text();
        c.move(0, 0);
        CHECK_THROWS_AS(c.fill_colour(1, 0, 0), std::logic_error); // rg while the path is open
        c.clip();
        CHECK_THROWS_AS(c.line(5, 5), std::logic_error);        // after W comes the painting operator
        c.end_path();
        CHECK_THROWS_AS(c.fill_colour(1.5, 0, 0), std::invalid_argument);
        CHECK(c.complete());
        c.save();
        CHECK(!c.complete());
        c.restore();

        // The font is part of the graphics state: Q brings back the one before q.
        content state{842};
        state.save();
        state.font("F1", 10);
        state.restore();
        state.begin_text();
        CHECK_THROWS_AS(state.show("x"), std::logic_error);
        state.font("F1", 10);
        state.show("x");
        state.end_text();
    }

    SECTION("Every built-in font, on one page");
    const auto fonts_file = output() / "fonts.pdf";
    {
        document pdf{fonts_file, {595.2756, 841.8898}}; // any size in points: A4 to the hundredth of a millimetre
        auto &page = pdf.page_content();
        double y = 42; // from the top
        for (const auto font: sc::standard_font::faces) {
            const auto resource = pdf.standard_font(font);
            page.begin_text();
            page.font(resource, 14);
            page.text_position(50, y);
            page.show(*document::win_ansi(std::string{sc::standard_font::name(font)} + ": Caf\xC3\xA9 (\xE2\x82\xAC 5) \\ fin"));
            page.end_text();
            y += 24;
        }
        CHECK_EQ(pdf.standard_font(sc::standard_font::face::helvetica), std::string{"F1"}); // the same font, the same name
        pdf.info("Title", "Built-in fonts \xE2\x80\x93 a test"); // an en dash: UTF-16
        pdf.info("Author", "simply-cpp");
        pdf.close();
        CHECK(pdf.closed());
    }
    {
        const auto file = read(fonts_file);
        const auto problem = structure_problem(file);
        CHECK_MSG(problem.empty(), problem);
        CHECK(contains(file, "/BaseFont /Times-BoldItalic /Encoding /WinAnsiEncoding"));
        CHECK(contains(file, "/Title <FEFF"));
        CHECK(contains(file, "/Author (simply-cpp)"));
        CHECK(contains(file, "/Producer (simply-cpp-pdflite)"));
        CHECK(contains(file, "/CreationDate (D:"));
        const auto streams = inflated_streams(file);
        CHECK(contains(streams, "/F1 14 Tf"));
        CHECK(contains(streams, "(Helvetica: Caf\\351 \\(\\200 5\\) \\\\ fin) Tj")); // é and € as octal, ( ) \\ escaped
    }

    SECTION("Pages, drawing on an earlier one again, shapes and clipping");
    const auto pages_file = output() / "pages.pdf";
    {
        document pdf{pages_file, sc::page_size::letter}; // 612 x 792 points
        pdf.add_page();
        pdf.add_page();
        CHECK_EQ(pdf.page_count(), std::size_t{3});
        CHECK_EQ(pdf.page(), std::size_t{3});
        pdf.set_page(1);
        auto &first = pdf.page_content();
        first.save();
        first.fill_colour(0.2, 0.4, 0.6);
        first.stroke_colour(0, 0, 0);
        first.line_width(2);
        first.rectangle(72, 92, 200, 100);                // y down: written from the bottom, 792 - 92 - 100
        first.fill_and_stroke();
        first.move(300, 192);
        first.curve(350, 92, 450, 92, 500, 192);
        first.close_path();
        first.fill(true);
        first.restore();
        first.save();
        first.rectangle(72, 620, 100, 100);
        first.clip();
        first.end_path();
        first.transform(1, 0, 0, 1, 10, 10); // right and down
        first.transform(2, 0, 0, 2, 0, 0);   // twice the size, about the top-left corner
        first.move(0, 0);
        first.line(300, 300);
        first.stroke();
        first.restore();
        pdf.set_page(3);
        const auto font = pdf.standard_font(sc::standard_font::face::courier);
        auto &third = pdf.page_content();
        third.begin_text();
        third.font(font, 10);
        third.text_position(72, 72); // the baseline an inch from the top
        third.show("Page three");
        third.end_text();
        const auto half = pdf.graphics_state(0.5, 0.25); // fills at half, strokes at a quarter
        CHECK_EQ(half, "GS1");
        CHECK_EQ(pdf.graphics_state(0.5, 0.25), "GS1"); // the same one again
        CHECK_EQ(pdf.graphics_state(0.75), "GS2");
        CHECK_THROWS_AS(pdf.graphics_state(1.5), std::invalid_argument);
        third.save();
        third.state(half);
        third.rectangle(72, 100, 50, 50);
        third.fill();
        third.restore();
        CHECK_THROWS_AS(pdf.set_page(4), std::out_of_range);
        CHECK_THROWS_AS(pdf.set_page(0), std::out_of_range);
        CHECK_THROWS_AS(pdf.info("ModDate", "x"), std::invalid_argument);
        pdf.close();
        CHECK_THROWS_AS(pdf.add_page(), std::logic_error);
    }
    {
        const auto file = read(pages_file);
        const auto problem = structure_problem(file);
        CHECK_MSG(problem.empty(), problem);
        CHECK(contains(file, "/Count 3 "));
        CHECK(contains(file, "/MediaBox [0 0 612 792]"));
        const auto streams = inflated_streams(file);
        CHECK(contains(streams, "0.2 0.4 0.6 rg"));
        CHECK(contains(streams, "300 600 m\n350 700 450 700 500 600 c\nh\nf*"));
        CHECK(contains(streams, "72 72 100 100 re\nW\nn"));
        CHECK(contains(streams, "72 600 200 100 re\n"));
        CHECK(contains(streams, "1 0 0 1 10 -10 cm"));  // down is y down in PDF's terms
        CHECK(contains(streams, "2 0 0 2 0 -792 cm"));  // the top-left corner stays where it is
        CHECK(contains(streams, "1 0 0 1 72 720 Tm"));
        CHECK(contains(streams, "(Page three) Tj"));
        CHECK(contains(streams, "q\n/GS1 gs\n"));
        CHECK(contains(file, "/ExtGState << /GS1 6 0 R /GS2 7 0 R >>"));
        CHECK(contains(file, "<< /Type /ExtGState /ca 0.5 /CA 0.25 >>"));
        CHECK(contains(file, "<< /Type /ExtGState /ca 0.75 /CA 1 >>"));
    }

    SECTION("Distances: millimetres and inches, written as points");
    const auto units_file = output() / "units.pdf";
    {
        document pdf{units_file, {sc::dist::mm(210), sc::dist::mm(297)}}; // A4 to the millimetre
        CHECK_NEAR(pdf.size().width(), 595.2756, 1e-4);
        auto &page = pdf.page_content();
        page.line_width(sc::dist::mm(0.5));
        page.rectangle(sc::dist::mm(10), sc::dist::mm(10), sc::dist::inch(1), 72); // mixed with a plain number of points
        page.stroke();
        const auto font = pdf.standard_font(sc::standard_font::face::helvetica);
        page.begin_text();
        page.font(font, sc::dist::mm(5));
        page.text_position(sc::dist::inch(1), sc::dist::inch(1));
        page.show("An inch in");
        page.end_text();
        pdf.link(sc::rect{sc::dist::mm(10), sc::dist::mm(10), sc::dist::inch(1), 72}, "https://example.com");
        pdf.bookmark("A centimetre down", 1, sc::dist::mm(10));
        pdf.close();
    }
    {
        const auto file = read(units_file);
        CHECK(structure_problem(file).empty());
        CHECK(contains(file, "/MediaBox [0 0 595.2756 841.8898]"));
        CHECK(contains(file, "/Rect [28.3465 741.5433 100.3465 813.5433]")); // 841.8898 - 28.3465 - 72 and up
        CHECK(contains(file, "/XYZ 0 813.5433 null]"));
        const auto streams = inflated_streams(file);
        CHECK(contains(streams, "1.4173 w"));                          // 0.5 mm
        CHECK(contains(streams, "28.3465 741.5433 72 72 re"));
        CHECK(contains(streams, "/F1 14.1732 Tf"));                    // 5 mm
        CHECK(contains(streams, "1 0 0 1 72 769.8898 Tm"));
    }

    SECTION("Pages of different sizes in one file");
    const auto sizes_file = output() / "sizes.pdf";
    {
        document pdf{sizes_file, sc::page_size::A4};
        CHECK_NEAR(pdf.size().width(), 595.2756, 1e-4);
        pdf.link(sc::rect{72, 72, 200, 20}, 2);            // to the top of page 2
        pdf.add_page(sc::page_size::A3, sc::orientation::landscape);
        CHECK_NEAR(pdf.size().width(), 1190.5512, 1e-4);          // the current page's size
        CHECK_NEAR(pdf.size().height(), 841.8898, 1e-4);
        pdf.page_content().rectangle(0, 0, pdf.size().width(), 100); // from its own top-left corner
        pdf.page_content().fill();
        pdf.bookmark("A3, 100 points down", 2, 100);
        pdf.add_page({200, 300});                             // any size
        pdf.add_page();                                     // the document's: A4 again
        CHECK_NEAR(pdf.size().height(), 841.8898, 1e-4);
        pdf.set_page(1);
        CHECK_NEAR(pdf.size().width(), 595.2756, 1e-4);
        CHECK_THROWS_AS(pdf.add_page({2, 300}), std::invalid_argument);
        pdf.close();
    }
    {
        const auto file = read(sizes_file);
        CHECK(structure_problem(file).empty());
        CHECK(contains(file, "/MediaBox [0 0 595.2756 841.8898]"));
        CHECK(contains(file, "/MediaBox [0 0 1190.5512 841.8898]"));
        CHECK(contains(file, "/MediaBox [0 0 200 300]"));
        CHECK(contains(file, "/Rect [72 749.8898 272 769.8898]")); // page 1's own height
        CHECK(contains(file, "/XYZ 0 741.8898 null]"));            // page 2's height, 100 down
        CHECK(contains(inflated_streams(file), "0 741.8898 1190.5512 100 re"));
    }

    SECTION("JPEG headers: the size and colours read, what PDF can't take refused");
    {
        // A JPEG's start: SOI, segments, then a frame header (the image data after it isn't read).
        const auto header = [](const int frame, const int precision, const int height, const int width, const int components,
                             const bool adobe = false) {
            std::string bytes{"\xFF\xD8"};
            bytes += std::string{"\xFF\xE0\x00\x10JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00", 18};
            if (adobe) bytes += std::string{"\xFF\xEE\x00\x0E" "Adobe" "\x00\x64\x00\x00\x00\x00\x00", 16};
            bytes += static_cast<char>(0xFF);
            bytes += static_cast<char>(frame);
            const int length = 8 + 3 * components;
            for (const int b: {length >> 8, length & 0xFF, precision, height >> 8, height & 0xFF, width >> 8, width & 0xFF, components})
                bytes += static_cast<char>(b);
            for (int c = 0; c < components; ++c) bytes += std::string{"\x01\x11\x00", 3};
            return bytes + std::string{"\xFF\xD9", 2};
        };
        const auto rgb = jpeg(header(0xC0, 8, 480, 640, 3));
        CHECK_EQ(rgb.size().width(), 640);
        CHECK_EQ(rgb.size().height(), 480);
        CHECK_EQ(rgb.components(), 3);
        CHECK_EQ(jpeg(header(0xC2, 8, 10, 20, 1)).components(), 1); // progressive, grey
        CHECK_EQ(jpeg(header(0xC0, 8, 10, 20, 4, true)).components(), 4); // Adobe CMYK
        CHECK_THROWS_AS(jpeg("not a JPEG"), std::invalid_argument);
        CHECK_THROWS_AS(jpeg(header(0xC3, 8, 10, 20, 3)), std::invalid_argument);  // lossless
        CHECK_THROWS_AS(jpeg(header(0xC9, 8, 10, 20, 3)), std::invalid_argument);  // arithmetic-coded
        CHECK_THROWS_AS(jpeg(header(0xC0, 12, 10, 20, 3)), std::invalid_argument); // 12-bit samples
        CHECK_THROWS_AS(jpeg(header(0xC0, 8, 0, 20, 3)), std::invalid_argument);   // the height later (DNL)
        CHECK_THROWS_AS(jpeg(header(0xC0, 8, 10, 20, 2)), std::invalid_argument);  // two components
        CHECK_THROWS_AS(jpeg(std::string{"\xFF\xD8\xFF\xC0\x00", 5}), std::invalid_argument); // cut short
        CHECK_THROWS_AS(jpeg::load(output() / "no-such.jpg"), std::runtime_error);

        // Written with its colour space, and CMYK from Adobe with the Decode array undoing its inversion.
        const auto cmyk_file = fs::temp_directory_path() / "sc-pdflite-cmyk.pdf"; // the image data isn't real: not for the checkers
        {
            document pdf{cmyk_file, {200, 200}};
            pdf.page_content().draw(pdf.image(jpeg(header(0xC0, 8, 10, 20, 4, true))), sc::rect{0, 0, 20, 10});
            pdf.close();
        }
        const auto cmyk = read(cmyk_file);
        CHECK(contains(cmyk, "/Subtype /Image /Width 20 /Height 10 /ColorSpace /DeviceCMYK /BitsPerComponent 8 /Decode [1 0 1 0 1 0 1 0] /Filter /DCTDecode /Length"));
        fs::remove(cmyk_file);
    }

    SECTION("JPEG images drawn: written once, placed through a transform");
    const auto images_file = output() / "images.pdf";
    {
        const auto photo = jpeg::load(SC_PDFLITE_TEST_PHOTO);
        CHECK_EQ(photo.size().width(), 960);
        CHECK_EQ(photo.size().height(), 640);
        document pdf{images_file, sc::page_size::A4};
        auto &page = pdf.page_content();
        const auto name = pdf.image(photo);
        CHECK_EQ(name, std::string{"Im1"});
        CHECK_EQ(pdf.image(jpeg{photo}), std::string{"Im1"}); // a copy: the same image
        page.draw(name, sc::rect{100, 100, 300, 200});
        page.draw(name, sc::transform{300, 0, 0, 200, 100, 400}.then(sc::transform::flip_horizontal(250)));
        page.begin_text();
        CHECK_THROWS_AS(page.draw(name, sc::rect{0, 0, 10, 10}), std::logic_error); // not in a text object
        page.end_text();
        pdf.add_page();
        pdf.page_content().draw(pdf.image(photo), sc::rect{0, 0, 96, 64}); // on another page: written once still
        pdf.close();
    }
    {
        const auto file = read(images_file);
        CHECK(structure_problem(file).empty());
        std::size_t images = 0;
        for (auto at = file.find("/Subtype /Image"); at != std::string::npos; at = file.find("/Subtype /Image", at + 1)) ++images;
        CHECK_EQ(images, std::size_t{1});
        CHECK(contains(file, "/Width 960 /Height 640 /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode /Length 105500"));
        CHECK(contains(file, "/XObject << /Im1 "));
        const auto streams = inflated_streams(file);
        CHECK(contains(streams, "q\n300 0 0 200 100 541.8898 cm\n/Im1 Do\nQ")); // 841.8898 - 100 - 200
        CHECK(contains(streams, "-300 0 0 200 400 241.8898 cm"));                 // mirrored about x = 250: 841.8898 - 200 - 400
    }

    SECTION("PNG: copied as it is when it can be, else decoded, with transparency as a soft mask");
    const auto pngs_file = output() / "pngs.pdf";
    {
        // The pattern tests/resource/png/make-pngs.py drew them with.
        const auto rgb = [](const int x, const int y) { return std::array<int, 3>{x * 30 % 256, y * 50 % 256, (x + y) * 10 % 256}; };
        const auto alpha = [](const int x, const int y) { return (x * y * 13 + 40) % 256; };
        const auto grey = [](const int x, const int y, const int depth) { return (x + 2 * y) % (1 << depth); };
        const auto palette = [](const int i) { return std::array<int, 3>{i * 36 % 256, i * 18 % 256, 255 - i * 30}; };
        const auto bytes_of = [](const auto &pixel) {
            std::string out;
            for (int y = 0; y < 5; ++y)
                for (int x = 0; x < 7; ++x)
                    for (const int v: pixel(x, y)) out += static_cast<char>(v);
            return out;
        };
        const fs::path folder = SC_PDFLITE_TEST_PNGS;
        document pdf{pngs_file, sc::page_size::A4};
        double y = 20;
        std::map<std::string, png> pictures;
        for (const char *name: {"rgb8", "rgb16", "grey1", "grey4", "palette4", "rgba8", "grey-alpha16", "palette-alpha", "grey-key",
                                "rgb-interlaced", "rgba-interlaced"}) {
            const auto picture = png::load(folder / (std::string{name} + ".png"));
            CHECK_EQ(picture.size().width(), 7.0);
            CHECK_EQ(picture.size().height(), 5.0);
            pdf.page_content().draw(pdf.image(picture), sc::rect{20, y, 70, 50});
            pictures.emplace(name, picture);
            y += 60;
        }
        CHECK(pictures.at("rgb8").copied());
        CHECK(pictures.at("rgb16").copied());
        CHECK(pictures.at("palette4").copied());
        CHECK(!pictures.at("rgba8").copied());          // an alpha channel
        CHECK(!pictures.at("grey-key").copied());       // a key colour
        CHECK(!pictures.at("rgb-interlaced").copied()); // interlaced
        CHECK(pictures.at("rgb-interlaced").interlaced());
        CHECK_EQ(pictures.at("grey-alpha16").bit_depth(), 16);
        CHECK_EQ(pictures.at("palette-alpha").colour_type(), 3);
        pdf.close();

        const auto file = read(pngs_file);
        CHECK(structure_problem(file).empty());
        CHECK(contains(file, "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /FlateDecode /DecodeParms << /Predictor 15 /Colors 3 /BitsPerComponent 8 /Columns 7 >>"));
        CHECK(contains(file, "/BitsPerComponent 16 /Filter /FlateDecode /DecodeParms << /Predictor 15 /Colors 3 /BitsPerComponent 16 /Columns 7 >>"));
        CHECK(contains(file, "/ColorSpace /DeviceGray /BitsPerComponent 1 /Filter /FlateDecode /DecodeParms << /Predictor 15 /Colors 1 /BitsPerComponent 1 /Columns 7 >>"));
        CHECK(contains(file, "/ColorSpace [/Indexed /DeviceRGB 7 <"));
        CHECK(contains(file, "/SMask "));
        const auto streams = inflated_streams(file);
        const auto expect = [&](const std::string &what, const std::string &bytes) { CHECK_MSG(contains(streams, bytes), what); };
        expect("rgba8 colours", bytes_of(rgb));
        expect("rgba8 alpha", bytes_of([&](const int x, const int y) { return std::array<int, 1>{alpha(x, y)}; }));
        expect("grey-alpha16: grey at 8 bits", bytes_of([&](const int x, const int y) { return std::array<int, 1>{grey(x, y, 8)}; }));
        expect("palette-alpha: the palette's colours",
               bytes_of([&](const int x, const int y) { return palette((x + y) % 8); }));
        expect("palette-alpha: its alphas, opaque past tRNS", bytes_of([&](const int x, const int y) {
            const int index = (x + y) % 8;
            return std::array<int, 1>{index < 4 ? std::array<int, 4>{0, 64, 128, 255}[index] : 255};
        }));
        expect("grey-key: grey 4 bits scaled to 8", bytes_of([&](const int x, const int y) { return std::array<int, 1>{grey(x, y, 4) * 17}; }));
        expect("grey-key: the key colour see-through",
               bytes_of([&](const int x, const int y) { return std::array<int, 1>{grey(x, y, 4) == 3 ? 0 : 255}; }));
    }
    {
        // Mistakes: each refused when the PNG is made.
        const auto good = read(fs::path{SC_PDFLITE_TEST_PNGS} / "rgb8.png");
        const auto chunk = [](const std::string &type, const std::string &body) {
            std::string out;
            for (const int shift: {24, 16, 8, 0}) out += static_cast<char>(body.size() >> shift & 0xFF);
            out += type + body;
            const auto crc = crc32(crc32(0, nullptr, 0), reinterpret_cast<const Bytef *>(out.data() + 4), static_cast<uInt>(type.size() + body.size()));
            for (const int shift: {24, 16, 8, 0}) out += static_cast<char>(crc >> shift & 0xFF);
            return out;
        };
        const auto idat = good.find("IDAT") - 4; // where the image data's chunk starts
        CHECK_THROWS_AS(png{"not a PNG"}, std::invalid_argument);
        std::string broken = good;
        broken[20] ^= 1; // inside IHDR: its CRC fails
        CHECK_THROWS_AS(png{broken}, std::invalid_argument);
        CHECK_THROWS_AS(png{good.substr(0, good.size() - 6)}, std::invalid_argument); // cut short
        CHECK_THROWS_AS(png{good.substr(0, idat) + chunk("QXYZ", "") + good.substr(idat)}, std::invalid_argument); // unknown critical
        CHECK_NOTHROW(png{good.substr(0, idat) + chunk("abCd", "ignored") + good.substr(idat)});              // unknown ancillary
        CHECK_NOTHROW(png{good.substr(0, idat) + chunk("PLTE", std::string(6, '\0')) + good.substr(idat)}); // RGB may suggest a palette
        const auto grey_png = read(fs::path{SC_PDFLITE_TEST_PNGS} / "grey4.png");
        const auto grey_idat = grey_png.find("IDAT") - 4;
        CHECK_THROWS_AS(png{grey_png.substr(0, grey_idat) + chunk("PLTE", std::string(6, '\0')) + grey_png.substr(grey_idat)}, std::invalid_argument); // grey can't
        const auto iend = good.find("IEND") - 4;
        CHECK_THROWS_AS(png{good.substr(0, iend) + chunk("tRNS", std::string(6, '\0')) + good.substr(iend)}, std::invalid_argument); // after IDAT
        CHECK_THROWS_AS(png::load(output() / "no-such.png"), std::runtime_error);
    }

    SECTION("SVG: a form of vectors, placed like a picture");
    const auto svg_file = output() / "svg.pdf";
    {
        const auto picture = sc::base64::encode(read(fs::path{SC_PDFLITE_TEST_PNGS} / "rgba8.png"));
        const std::string text = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100" viewBox="0 0 200 100">
            <rect x="5" y="5" width="190" height="90" rx="12" fill="#2a6f97" stroke="#000000" stroke-width="2"/>
            <circle cx="50" cy="50" r="30" fill="#f4d35e" fill-opacity="0.5"/>
            <path d="M100 20 h60 v60 h-60 Z M115 35 h30 v30 h-30 Z" fill="#ee6c4d" fill-rule="evenodd"/>
            <clipPath id="c"><rect x="170" y="10" width="20" height="20"/></clipPath>
            <circle cx="180" cy="20" r="15" fill="#ffffff" clip-path="url(#c)"/>
            <text x="10" y="90" font-family="Arial" font-weight="bold" font-size="12" fill="#ffffff">Caf&#233; &#8364;5 &#1046;</text>
            <text x="120" y="95" font-family="Unknown Sans" font-size="8" fill="#ffffff">fallback</text>
            <image x="160" y="60" width="35" height="25" href="data:image/png;base64,)svg" + picture + R"svg("/>
        </svg>)svg";
        const auto drawing = svg::parse(text);
        CHECK_EQ(drawing.size().width(), 200.0);
        CHECK_EQ(drawing.size().height(), 100.0);
        document pdf{svg_file, sc::page_size::A4};
        const auto name = pdf.image(drawing);
        CHECK_EQ(name, std::string{"Fm1"});
        pdf.page_content().draw(name, sc::rect{50, 50, 400, 200});
        pdf.page_content().draw(name, sc::transform{200, 0, 0, 100, 50, 300}.then(sc::transform::rotate(10, {150, 350})));
        CHECK_EQ(pdf.image(svg{drawing}), std::string{"Fm1"}); // a copy: the same drawing
        pdf.close();
        CHECK_THROWS_AS(svg::parse("<svg xmlns=\"http://www.w3.org/2000/svg\"/>"), std::invalid_argument); // no size
    }
    {
        const auto file = read(svg_file);
        CHECK(structure_problem(file).empty());
        std::size_t forms = 0;
        for (auto at = file.find("/Subtype /Form"); at != std::string::npos; at = file.find("/Subtype /Form", at + 1)) ++forms;
        CHECK_EQ(forms, std::size_t{1});                              // drawn twice, written once
        CHECK(contains(file, "/BBox [0 0 200 100] /Matrix [0.005 0 0 0.01 0 0]"));
        CHECK(contains(file, "/BaseFont /Helvetica-Bold"));            // Arial bold
        CHECK(contains(file, "/BaseFont /Helvetica "));                // an unknown family: Helvetica
        CHECK(contains(file, "/ExtGState << /GS1 << /Type /ExtGState /ca 0.5 /CA 1 >>"));
        CHECK(contains(file, "/XObject << /Im1 "));                    // the PNG inside it
        CHECK(contains(file, "/SMask "));                              // with its alpha
        const auto streams = inflated_streams(file);
        CHECK(contains(streams, "/GS1 gs"));
        CHECK(contains(streams, "f*"));                               // the even-odd ring
        CHECK(contains(streams, "W\nn"));                             // the clip
        CHECK(contains(streams, "(Caf\\351 \\2005 ?) Tj"));          // \u00e9 and \u20ac in WinAnsi, \u0416 as '?'
        CHECK(contains(streams, "/Fm1 Do"));
        CHECK(contains(streams, "400 0 0 200 50 591.8898 cm"));         // the unit square into 400 x 200 at (50, 50)
    }

    SECTION("Embedded fonts: subsets read back by FreeType, glyph for glyph as the font's");
    {
        // The subset (FontFile2) of the first embedded font in a file.
        const auto subset_of = [](const std::string &file) {
            static const std::regex head{R"(/Length1 (\d+) /Length (\d+) /Filter /FlateDecode >>\nstream\n)"};
            std::smatch found;
            if (!std::regex_search(file, found, head)) return std::string{};
            const auto length1 = std::stoul(found[1]), length = std::stoul(found[2]);
            std::string program(length1, '\0');
            uLongf size = length1;
            const auto at = static_cast<std::size_t>(found.position() + found.length());
            if (uncompress(reinterpret_cast<Bytef *>(program.data()), &size, reinterpret_cast<const Bytef *>(file.data() + at), length) != Z_OK) return std::string{};
            return program;
        };
        // Whether every glyph of text in the subset has the original's advance and outline extent (to a
        // unit), the subset's glyphs being numbered in order of first use.
        const auto check = [&](const std::string &what, const std::filesystem::path &font_file, const long index, const std::string &text) {
            const auto out = output() / ("font-" + what + ".pdf");
            {
                document pdf{out, sc::page_size::A5};
                const auto embedded = font::load(font_file, index);
                auto &page = pdf.page_content();
                page.begin_text();
                page.font(pdf.font(embedded), 20);
                page.text_position(20, 60);
                page.show(embedded.encode(text));
                page.end_text();
                pdf.close();
            }
            const auto file = read(out);
            CHECK_MSG(structure_problem(file).empty(), what);
            CHECK_MSG(contains(file, "/Subtype /Type0") && contains(file, "/Encoding /Identity-H") && contains(file, "/CIDToGIDMap /Identity"), what);
            CHECK_MSG(contains(file, "/ToUnicode "), what);
            const auto program = subset_of(file);
            CHECK_MSG(!program.empty(), what + ": the subset");
            FT_Library library;
            FT_Init_FreeType(&library);
            FT_Face original, subset;
            CHECK(FT_New_Face(library, font_file.string().c_str(), index, &original) == 0);
            CHECK_MSG(FT_New_Memory_Face(library, reinterpret_cast<const FT_Byte *>(program.data()), static_cast<FT_Long>(program.size()), 0, &subset) == 0,
                      what + ": FreeType reads the subset");
            std::vector<FT_UInt> order{0};
            for (const char32_t c: code_points(text)) {
                const auto glyph = FT_Get_Char_Index(original, c);
                if (std::ranges::find(order, glyph) == order.end()) order.push_back(glyph);
            }
            CHECK_EQ(static_cast<std::size_t>(subset->num_glyphs), order.size());
            int mismatched = 0;
            for (std::size_t g = 0; g < order.size(); ++g) {
                FT_Load_Glyph(original, order[g], FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING);
                FT_BBox a;
                FT_Outline_Get_CBox(&original->glyph->outline, &a);
                const auto advance = original->glyph->metrics.horiAdvance;
                FT_Load_Glyph(subset, static_cast<FT_UInt>(g), FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING);
                FT_BBox b;
                FT_Outline_Get_CBox(&subset->glyph->outline, &b);
                // The control box: a CFF glyph's cubic controls aren't kept, so its box may shrink a little.
                const bool same = subset->glyph->metrics.horiAdvance == advance && std::abs(a.yMax - b.yMax) <= 40 && std::abs(a.xMin - b.xMin) <= 40;
                if (!same) ++mismatched;
            }
            CHECK_MSG(mismatched == 0, what + ": " + std::to_string(mismatched) + " glyphs differ");
            FT_Done_Face(subset);
            FT_Done_Face(original);
            FT_Done_FreeType(library);
        };

        if (const auto face = sc::font{"sans-serif", 12}.face()) {
            check("installed", face->file(), face->index(), "Hello, caf\u00e9 \u20ac \u0416!");
            const font embedded{sc::font{"sans-serif", 12}};
            CHECK(embedded.covers("Hello"));
            CHECK(!embedded.covers("\U0010FFFD"));                                // a private-use code point: no glyph
            CHECK_EQ(embedded.encode("HH").size(), std::size_t{4});               // two bytes a glyph
            CHECK_EQ(embedded.encode("H"), embedded.encode("H"));                 // the same glyph, the same number
            CHECK(embedded.width("HH", 10) > embedded.width("H", 10));
            CHECK(embedded.ascender(10) > 0 && embedded.descender(10) > 0 && embedded.cap_height(10) > 0);
            CHECK(embedded.x_height(10) > 0 && embedded.x_height(10) < embedded.cap_height(10));
            CHECK(embedded.cap_height(10) < embedded.ascender(10));
            CHECK_THROWS_AS(embedded.encode("\xC3"), std::invalid_argument);
        } else std::cout << "no font from fontconfig: the installed-font test skipped\n";
        CHECK_THROWS_AS(font::load(output() / "no-such.ttf"), std::runtime_error);

        const fs::path variable = SC_PDFLITE_TEST_VARIABLE_FONT;
        if (fs::exists(variable)) {
            // Its named instances: the bold one's glyphs are bold, as the subset shows.
            FT_Library library;
            FT_Init_FreeType(&library);
            FT_Face probe;
            FT_New_Face(library, variable.string().c_str(), 0, &probe);
            const auto instances = probe->style_flags >> 16;
            FT_Done_Face(probe);
            long bold = -1, regular = -1;
            for (long i = 1; i <= instances; ++i) {
                FT_Face named;
                FT_New_Face(library, variable.string().c_str(), i << 16, &named);
                const std::string style = named->style_name ? named->style_name : "";
                if (style == "Bold") bold = i << 16;
                if (style == "Regular") regular = i << 16;
                FT_Done_Face(named);
            }
            FT_Done_FreeType(library);
            CHECK(bold > 0 && regular > 0);
            if (bold > 0 && regular > 0) {
                CHECK(font::load(variable, bold).width("Hamburg", 10) > font::load(variable, regular).width("Hamburg", 10));
                check("variable-bold", variable, bold, "Hamburg Bold");
            }
        } else std::cout << "no variable font (" << variable << "): its test skipped\n";

        const fs::path cff = SC_PDFLITE_TEST_CFF_FONT;
        if (fs::exists(cff)) check("cff", cff, 0, "Curves: S, O, g & @ 8");
        else std::cout << "no CFF font (" << cff << "): its test skipped\n";
    }

    SECTION("Passwords and permissions: AES-256, opened again with either password");
    {
        CHECK_EQ(permissions{}.bits(), static_cast<std::int32_t>(0xFFFFFAC4u));        // print (well), accessibility
        CHECK_EQ((permissions{false, false}).bits(), static_cast<std::int32_t>(0xFFFFF2C0u)); // nothing but accessibility
        CHECK_THROWS_AS(encryption(protection{}), std::invalid_argument);           // no password: nothing to protect

        const auto protected_file = output() / "protected.pdf";
        {
            document pdf{protected_file, sc::page_size::A5};
            const auto font = pdf.standard_font(sc::standard_font::face::helvetica);
            auto &page = pdf.page_content();
            page.begin_text();
            page.font(font, 14);
            page.text_position(40, 60);
            page.show("Secret plans");
            page.end_text();
            pdf.link(sc::rect{40, 40, 100, 20}, "https://example.com/hidden");
            pdf.info("Title", "Top secret title");
            pdf.protect({"open sesame", "owner-only", permissions{true, false}});   // print, at low resolution
            pdf.close();
        }
        std::ofstream{protected_file.string() + ".password"} << "open sesame"; // for the checkers

        const auto file = read(protected_file);
        CHECK(structure_problem(file).empty());
        CHECK(contains(file, "/Filter /Standard /V 5 /R 6 /Length 256 /CF << /StdCF << /AuthEvent /DocOpen /CFM /AESV3 /Length 32 >> >>"));
        CHECK(contains(file, "/Extensions << /ADBE << /BaseVersion /1.7 /ExtensionLevel 8 >> >>"));
        CHECK(contains(file, "/Encrypt "));
        CHECK(!contains(file, "Top secret title"));                                 // the strings are encrypted
        CHECK(!contains(file, "example.com"));
        CHECK(contains(file, "/P -3388 "));                                          // 0xFFFFF2C4: print, low resolution

        // The /Encrypt dictionary's strings, as written (hex, unencrypted).
        const auto hex_of = [&](const std::string &key) {
            std::smatch found;
            const std::regex pattern{"/" + key + " <([0-9A-F]+)>"};
            std::string bytes;
            if (std::regex_search(file, found, pattern))
                for (std::size_t i = 0; i + 1 < found[1].length(); i += 2) bytes += static_cast<char>(std::stoi(found[1].str().substr(i, 2), nullptr, 16));
            return bytes;
        };
        const dictionary encrypt{{"R", 6}, {"U", string{hex_of("U"), true}}, {"O", string{hex_of("O"), true}},
                                 {"UE", string{hex_of("UE"), true}}, {"OE", string{hex_of("OE"), true}}};
        const auto by_user = encryption::key_for(encrypt, "open sesame");
        const auto by_owner = encryption::key_for(encrypt, "owner-only");
        CHECK(by_user.has_value());
        CHECK(by_owner.has_value());
        CHECK(by_user == by_owner);                                                  // one file key, either password
        CHECK_EQ(by_user.value_or("").size(), std::size_t{32});
        CHECK(!encryption::key_for(encrypt, "wrong"));
        CHECK_THROWS_AS(encryption::key_for(dictionary{{"R", 4}}, "x"), std::invalid_argument);

        // The page's content: decrypted with the key, then inflated.
        bool found_text = false;
        static const std::regex stream_head{R"(/Length (\d+) /Filter /FlateDecode >>\nstream\n)"};
        for (std::sregex_iterator it{file.begin(), file.end(), stream_head}, end; it != end && by_user; ++it) {
            const auto data = file.substr(it->position() + it->length(), std::stoul((*it)[1]));
            const auto packed = encryption::decrypt(*by_user, data);
            std::string plain(1 << 16, '\0');
            uLongf size = plain.size();
            if (uncompress(reinterpret_cast<Bytef *>(plain.data()), &size, reinterpret_cast<const Bytef *>(packed.data()), packed.size()) == Z_OK)
                found_text = found_text || contains(plain.substr(0, size), "(Secret plans) Tj");
        }
        CHECK(found_text);
        CHECK_THROWS_AS(encryption::decrypt(std::string(32, 'x'), file.substr(0, 48)), std::invalid_argument); // not ours

        // Only an owner password: anyone opens it, the permissions apply.
        const auto owner_file = output() / "owner-only.pdf";
        {
            document pdf{owner_file, sc::page_size::A5};
            pdf.protect({"", "admin", permissions{}});
            pdf.close();
        }
        const auto owned = read(owner_file);
        const dictionary owner_encrypt{{"R", 6}, {"U", string{[&] { std::smatch m; std::regex_search(owned, m, std::regex{"/U <([0-9A-F]+)>"}); std::string b; for (std::size_t i = 0; i + 1 < m[1].length(); i += 2) b += static_cast<char>(std::stoi(m[1].str().substr(i, 2), nullptr, 16)); return b; }(), true}},
                                       {"O", string{std::string(48, '\0'), true}}, {"UE", string{std::string(32, '\0'), true}}, {"OE", string{std::string(32, '\0'), true}}};
        CHECK(encryption::key_for(owner_encrypt, "").has_value());                   // the empty user password opens it
    }

    SECTION("Links and bookmarks");
    const auto links_file = output() / "links.pdf";
    {
        document pdf{links_file, {595.2756, 841.8898}};
        const auto font = pdf.standard_font(sc::standard_font::face::helvetica);
        auto &first = pdf.page_content();
        first.begin_text();
        first.font(font, 12);
        first.text_position(72, 82);
        first.show("A link to example.com, and one to page 2");
        first.end_text();
        const double h = pdf.size().height();
        pdf.link(sc::rect{72, h - 775, 228, 20}, "https://example.com/a b/caf\xC3\xA9?x=(1)"); // a space, UTF-8 and brackets
        pdf.link(sc::rect{72, h - 750, 228, 20}, 2, h - 800); // a page that isn't there yet
        CHECK_THROWS_AS(pdf.link(sc::rect{std::numeric_limits<double>::quiet_NaN(), 0, 10, 10}, "https://example.com"), std::invalid_argument);
        CHECK_THROWS_AS(pdf.link(sc::rect{0, 0, 10, 10}, ""), std::invalid_argument);
        CHECK_THROWS_AS(pdf.link(sc::rect{0, 0, 10, 10}, 0, 0), std::out_of_range);
        pdf.bookmark("Chapter 1", 1);                 // from the top of the page
        pdf.bookmark("Section 1.1", 1, 140, 1);
        pdf.bookmark("Detail 1.1.1", 1, 190, 2);
        pdf.bookmark("Section 1.2 \xE2\x80\x93 caf\xC3\xA9", 2, 40, 1); // UTF-16
        pdf.bookmark("Chapter 2", 2);
        pdf.bookmark("Deep jump", 2, 440, 5); // more than one deeper: under Chapter 2
        pdf.add_page();
        pdf.close();
    }
    {
        const auto file = read(links_file);
        const auto problem = structure_problem(file);
        CHECK_MSG(problem.empty(), problem);
        CHECK(contains(file, "/S /URI /URI (https://example.com/a%20b/caf%C3%A9?x=\\(1\\))"));
        CHECK(contains(file, "/Subtype /Link /Rect [72 730 300 750] /F 4 /Border [0 0 0] /Dest ["));
        CHECK(contains(file, " /XYZ 0 800 null]"));     // h - (h - 800)
        CHECK(contains(file, " /XYZ 0 841.8898 null]")); // a bookmark to the top
        CHECK(contains(file, "/Annots ["));
        CHECK(contains(file, "/Type /Outlines /First "));
        CHECK(contains(file, "/Count 2 >>"));           // two chapters at the top
        CHECK(contains(file, "/Title (Chapter 1) /Parent "));
        CHECK(contains(file, "/Count -2 /Dest"));       // Chapter 1, closed, two sections
        CHECK(contains(file, "/PageMode /UseOutlines"));
        CHECK(contains(file, "/Title <FEFF"));
    }
    {
        // A link or bookmark to a page that never comes: close() refuses before writing anything.
        const auto missing = fs::temp_directory_path() / "sc-pdflite-missing-page.pdf"; // not for the checkers
        document pdf{missing, {200, 200}};
        pdf.link(sc::rect{0, 0, 10, 10}, 3);
        CHECK_THROWS_AS(pdf.close(), std::out_of_range);
        CHECK(!pdf.closed());
        pdf.add_page();
        pdf.add_page();
        pdf.bookmark("Nowhere", 9, 0);
        CHECK_THROWS_AS(pdf.close(), std::out_of_range);
    } // the destructor reports the same mistake on std::cerr
    fs::remove(fs::temp_directory_path() / "sc-pdflite-missing-page.pdf");

    SECTION("Mistakes");
    {
        {
            document landscape{fs::temp_directory_path() / "sc-pdflite-landscape.pdf", sc::page_size::A5, sc::orientation::landscape};
            CHECK_NEAR(landscape.size().width(), 595.2756, 1e-4); // A5 turned: 210 x 148 mm
            CHECK_NEAR(landscape.size().height(), 419.5276, 1e-4);
            CHECK_EQ(sc::console::format(static_cast<sc::dist::mm>(landscape.size().height())), std::string{"148mm"});
            landscape.close();
            fs::remove(fs::temp_directory_path() / "sc-pdflite-landscape.pdf");
        }
        CHECK_THROWS_AS(document(output() / "too-small.pdf", {2, 100}), std::invalid_argument);
        CHECK(!fs::exists(output() / "too-small.pdf")); // checked before the file is made
        const auto unfinished = fs::temp_directory_path() / "sc-pdflite-unfinished.pdf"; // not for the checkers
        document pdf{unfinished, {200, 200}};
        pdf.page_content().save();
        CHECK_THROWS_AS(pdf.close(), std::logic_error); // an open q
        pdf.page_content().restore();
        pdf.close();
        CHECK(structure_problem(read(unfinished)).empty());
        fs::remove(unfinished);
    }

    TEST_SUMMARY();
}
