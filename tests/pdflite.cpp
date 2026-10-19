// sc-pdflite: how values are spelt, the content stream's rules, and files
// whose structure is checked byte by byte here; ctest's check-* tests then run qpdf, pdfcpu and
// mutool over every file written to SC_PDFLITE_OUTPUT.
#include "pdflite.h"

#include "sc_test.h"

#include <zlib.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <regex>
#include <stdexcept>
#include <string>

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
    SECTION("Numbers: integers when whole, four decimals at most, no exponent");
    CHECK_EQ(number(0), std::string{"0"});
    CHECK_EQ(number(-0.00001), std::string{"0"}); // never -0
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
        document pdf{fonts_file, 595.2756, 841.8898}; // any size in points: A4 to the hundredth of a millimetre
        auto &page = pdf.page_content();
        double y = 42; // from the top
        for (const char *font: {"Helvetica", "Helvetica-Bold", "Helvetica-Oblique", "Helvetica-BoldOblique",
                                "Times-Roman", "Times-Bold", "Times-Italic", "Times-BoldItalic",
                                "Courier", "Courier-Bold", "Courier-Oblique", "Courier-BoldOblique"}) {
            const auto resource = pdf.standard_font(font);
            page.begin_text();
            page.font(resource, 14);
            page.text_position(50, y);
            page.show(*document::win_ansi(std::string{font} + ": Caf\xC3\xA9 (\xE2\x82\xAC 5) \\ fin"));
            page.end_text();
            y += 24;
        }
        CHECK_EQ(pdf.standard_font("Helvetica"), std::string{"F1"}); // the same font, the same name
        CHECK_THROWS_AS(pdf.standard_font("Arial"), std::invalid_argument);
        CHECK_THROWS_AS(pdf.standard_font("Symbol"), std::invalid_argument); // not WinAnsiEncoding
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
        const auto font = pdf.standard_font("Courier");
        auto &third = pdf.page_content();
        third.begin_text();
        third.font(font, 10);
        third.text_position(72, 72); // the baseline an inch from the top
        third.show("Page three");
        third.end_text();
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
    }

    SECTION("Distances: millimetres and inches, written as points");
    const auto units_file = output() / "units.pdf";
    {
        document pdf{units_file, sc::mm(210), sc::mm(297)}; // A4 to the millimetre
        CHECK_NEAR(pdf.width(), 595.2756, 1e-4);
        auto &page = pdf.page_content();
        page.line_width(sc::mm(0.5));
        page.rectangle(sc::mm(10), sc::mm(10), sc::inch(1), 72); // mixed with a plain number of points
        page.stroke();
        const auto font = pdf.standard_font("Helvetica");
        page.begin_text();
        page.font(font, sc::mm(5));
        page.text_position(sc::inch(1), sc::inch(1));
        page.show("An inch in");
        page.end_text();
        pdf.link(sc::rect{sc::mm(10), sc::mm(10), sc::inch(1), 72}, "https://example.com");
        pdf.bookmark("A centimetre down", 1, sc::mm(10));
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

    SECTION("Links and bookmarks");
    const auto links_file = output() / "links.pdf";
    {
        document pdf{links_file, 595.2756, 841.8898};
        const auto font = pdf.standard_font("Helvetica");
        auto &first = pdf.page_content();
        first.begin_text();
        first.font(font, 12);
        first.text_position(72, 82);
        first.show("A link to example.com, and one to page 2");
        first.end_text();
        const double h = pdf.height();
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
        document pdf{missing, 200, 200};
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
            CHECK_EQ(landscape.width(), 595.0); // sc-graphics' A5, turned: 595 x 420 points
            CHECK_EQ(landscape.height(), 420.0);
            landscape.close();
            fs::remove(fs::temp_directory_path() / "sc-pdflite-landscape.pdf");
        }
        CHECK_THROWS_AS(document(output() / "too-small.pdf", 2, 100), std::invalid_argument);
        CHECK(!fs::exists(output() / "too-small.pdf")); // checked before the file is made
        const auto unfinished = fs::temp_directory_path() / "sc-pdflite-unfinished.pdf"; // not for the checkers
        document pdf{unfinished, 200, 200};
        pdf.page_content().save();
        CHECK_THROWS_AS(pdf.close(), std::logic_error); // an open q
        pdf.page_content().restore();
        pdf.close();
        CHECK(structure_problem(read(unfinished)).empty());
        fs::remove(unfinished);
    }

    TEST_SUMMARY();
}
