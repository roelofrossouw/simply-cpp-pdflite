// Writes PDFs into the temporary directory and checks what the files hold: pages, sizes, links,
// fonts, images and SVG drawings, plus the mistakes that throw.
#include <sc/pdflite/writer.h>

#include "sc_test.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
    const auto directory = std::filesystem::temp_directory_path() / "sc-pdflite-writer-test";
    const std::filesystem::path resources{SC_PDFLITE_TEST_WRITER};

    std::string read(const std::filesystem::path &path) {
        std::ifstream file{path, std::ios::binary};
        return {std::istreambuf_iterator<char>{file}, {}};
    }

    bool contains(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }

    // Whether an object was written into the middle of another's stream (an earlier page's fonts
    // landing inside the current page's content, say).
    bool objects_inside_streams(const std::string &file) {
        for (auto start = file.find("stream\r\n"); start != std::string::npos; start = file.find("stream\r\n", start + 1)) {
            if (start >= 3 && file.compare(start - 3, 3, "end") == 0) continue;
            const auto end = file.find("endstream", start);
            if (end == std::string::npos || file.substr(start, end - start).find(" 0 obj") != std::string::npos) return true;
        }
        return false;
    }

    const std::string badge = R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100" viewBox="0 0 200 100">)"
                              R"(<rect x="5" y="5" width="190" height="90" rx="12" fill="#2a6f97" stroke="#000000"/>)"
                              R"(<circle cx="50" cy="50" r="30" fill="#f4d35e"/>)"
                              R"(<path d="M100 20 L180 20 L140 80 Z" fill="#ee6c4d"/></svg>)";
}

int main() {
    std::filesystem::create_directories(directory);

    SECTION("A page of everything");
    {
        const auto path = directory / "everything.pdf";
        {
            sc::pdflite::writer pdf{path};
            CHECK_EQ(pdf.page_count(), 1u);
            CHECK_NEAR(pdf.page_width(), 595.2756, 1e-4); // A4: 210 x 297 mm
            CHECK_NEAR(pdf.page_height(), 841.8898, 1e-4);

            CHECK_NOTHROW(pdf.draw(sc::graphics::text{"Hello, PDF", {50, 80}, {"sans-serif", 24}}));
            CHECK_NOTHROW(pdf.draw(sc::graphics::text{"Bold and blue", {50, 110}, sc::graphics::font::bold("sans-serif", 14)}, sc::color{0, 0, 1}));
            CHECK_NOTHROW(pdf.draw(sc::line{{50, 120}, {545, 120}}, sc::graphics::stroke{sc::color::Black, 0.5}));
            CHECK_NOTHROW(pdf.draw(sc::rounded_rect{sc::rect{50, 140, 200, 100}, 12}, sc::graphics::stroke{sc::color::Red, 2}));
            CHECK_NOTHROW(pdf.draw(sc::rect{300, 140, 200, 100}, sc::graphics::appearance::filled(sc::color{0.6, 0.95, 0.4})));
            CHECK_NOTHROW(pdf.draw(sc::rounded_rect{sc::rect{300, 140, 200, 100}, 6}, {sc::color::Black, sc::color::White}));
            CHECK_NOTHROW(pdf.draw(sc::circle{{400, 600}, 40}, {sc::graphics::stroke{sc::color::Black, 2}, sc::color{1, 0.8, 0.2}}));
            CHECK_NOTHROW(pdf.draw(sc::polygon{{300, 700}, {360, 640}, {420, 700}}, sc::color::Blue));
            CHECK_NOTHROW(pdf.draw(sc::polygon{{300.5, 760.25}, {360.75, 700.5}, {420.25, 760.75}}, sc::color::Blue));
            CHECK_NOTHROW(pdf.draw(sc::polygon_i{{300, 800}, {360, 740}, {420, 800}}, sc::color::Black));
            CHECK_NOTHROW(pdf.draw(sc::rotated_rect{{480, 600}, {60, 30}, 30}, sc::graphics::appearance::filled(sc::color::Green)));
            CHECK_NOTHROW(pdf.draw(sc::polygon{}, sc::color::Black)); // nothing to draw
            CHECK_NOTHROW(pdf.draw(sc::circle{{0, 0}, 0}, sc::color::Black));
            CHECK_NOTHROW(pdf.draw_image(resources / "image.png", {50, 260, 240, 160}));
            CHECK_NOTHROW(pdf.draw_image(resources / "image.jpg", {300, 260, 240, 160}));
            CHECK_NOTHROW(pdf.draw(sc::graphics::drawing::from_svg(badge), sc::rect{50, 440, 200, 100}));
            CHECK_NOTHROW(pdf.add_link("https://github.com/roelofrossouw/simply-cpp", {50, 60, 150, 30}));
            pdf.close();
            CHECK_NOTHROW(pdf.close()); // twice does nothing
            CHECK_THROWS_AS(pdf.draw(sc::graphics::text{"too late", {50, 600}}), std::logic_error);
        }
        const auto file = read(path);
        CHECK(file.starts_with("%PDF-1.7"));
        CHECK(contains(file, "%%EOF"));
        CHECK(contains(file, "/Count 1"));
        CHECK(std::regex_search(file, std::regex{R"(/MediaBox \[0 0 595\.27\d* 841\.8\d*\])"}));
        CHECK(contains(file, "/URI (https://github.com/roelofrossouw/simply-cpp)"));
        CHECK(contains(file, "/FontFile")); // the font is embedded
        CHECK(contains(file, "/Subtype /Image"));
    }

    SECTION("Pages");
    {
        const auto path = directory / "pages.pdf";
        {
            sc::pdflite::writer pdf{path, sc::graphics::page_size::letter};
            CHECK_EQ(pdf.page_width(), 612.0);
            CHECK_EQ(pdf.page_height(), 792.0);
            pdf.new_page();
            CHECK_EQ(pdf.page(), 2u);
            pdf.set_page(4); // adds pages 3 and 4
            CHECK_EQ(pdf.page_count(), 4u);
            CHECK_EQ(pdf.page(), 4u);
            pdf.draw(sc::graphics::text{"Page four", {72, 72}});
            pdf.set_page(1);
            pdf.draw(sc::graphics::text{"Back on page one", {72, 72}});
            pdf.add_link("https://example.com/one", {72, 60, 100, 20});
            CHECK_THROWS_AS(pdf.set_page(0), std::out_of_range);
        }
        const auto file = read(path);
        CHECK(!objects_inside_streams(file));
        CHECK(contains(file, "/Count 4"));
        CHECK(contains(file, "/MediaBox [0 0 612 792]"));
        CHECK(contains(file, "/URI (https://example.com/one)"));

        const auto two_path = directory / "two.pdf"; // ending on a later page, with fonts on both
        {
            sc::pdflite::writer two{two_path};
            two.draw(sc::graphics::text{"page one", {72, 72}});
            two.new_page();
            two.draw(sc::graphics::text{"page two", {72, 72}, sc::graphics::font::bold()});
        }
        CHECK(!objects_inside_streams(read(two_path)));

        sc::pdflite::writer legal{directory / "legal.pdf", sc::graphics::page_size::legal};
        CHECK_EQ(legal.page_height(), 1008.0);
        sc::pdflite::writer a5{directory / "a5.pdf", sc::graphics::page_size::A5};
        CHECK_NEAR(a5.page_width(), 419.5276, 1e-4); // 148 x 210 mm
        CHECK_NEAR(a5.page_height(), 595.2756, 1e-4);
        sc::pdflite::writer wide{directory / "a3-landscape.pdf", sc::graphics::page_size::A3, sc::graphics::orientation::landscape};
        CHECK_NEAR(wide.page_width(), 1190.5512, 1e-4); // 420 x 297 mm
        CHECK_NEAR(wide.page_height(), 841.8898, 1e-4);
    }

    SECTION("Clipping to an empty shape hides what's drawn, rather than failing");
    {
        const auto path = directory / "empty-clip.pdf";
        {
            sc::pdflite::writer pdf{path};
            pdf.push_clip(sc::path{});
            pdf.draw(sc::graphics::text{"hidden", {72, 72}});
            pdf.pop_clip();
            pdf.draw(sc::graphics::text{"shown", {72, 100}});
        }
        CHECK(!objects_inside_streams(read(path))); // written whole: push_clip() didn't throw
    }

    SECTION("Transparency: a colour's alpha");
    {
        const auto path = directory / "alpha.pdf";
        {
            sc::pdflite::writer pdf{path};
            pdf.draw(sc::circle{{100, 100}, 20}, sc::graphics::appearance::filled(sc::color{0, 0, 0, 0.5}));
            pdf.draw(sc::circle{{150, 100}, 20}, sc::graphics::appearance::filled(sc::color{0, 0, 0, 0.5})); // the same state again
            pdf.draw(sc::rect{50, 150, 100, 50}, sc::graphics::stroke{sc::color{1, 0, 0, 0.75}, 2});
            pdf.draw(sc::graphics::text{"faint", {50, 250}}, sc::color{0, 0, 1, 0.25});
            pdf.draw(sc::rect{50, 300, 100, 50}); // opaque: no state
        }
        const auto file = read(path);
        CHECK(contains(file, "/ExtGState"));
        CHECK(contains(file, "/ca 0.5"));
        CHECK(contains(file, "/CA 0.75"));
        CHECK(contains(file, "/ca 0.25"));
        std::size_t states = 0;
        for (auto at = file.find("/Type /ExtGState"); at != std::string::npos; at = file.find("/Type /ExtGState", at + 1)) ++states;
        CHECK_EQ(states, std::size_t{3});
        CHECK(!objects_inside_streams(file));
    }

    SECTION("Bitmaps: each written once, used from every page");
    {
        const auto path = directory / "bitmaps.pdf";
        {
            sc::pdflite::writer pdf{path};
            const auto png = sc::graphics::bitmap::load(resources / "image.png");
            const auto jpeg = sc::graphics::bitmap::load(resources / "image.jpg");
            CHECK_NOTHROW(pdf.draw(png, {50, 50, 240, 160}));
            CHECK_NOTHROW(pdf.draw(png, {300, 50, 120, 80})); // the same image again
            CHECK_NOTHROW(pdf.draw(jpeg, {50, 300, 240, 160}));
            pdf.new_page();
            CHECK_NOTHROW(pdf.draw(sc::graphics::bitmap{png}, {50, 50, 240, 160})); // a copy: the same bytes
            sc::graphics::drawing mirrored{{595, 842}};
            mirrored.draw(jpeg, {50, 300, 240, 160});
            mirrored.flip_horizontal();
            CHECK_NOTHROW(pdf.draw(mirrored));
        }
        const auto file = read(path);
        std::size_t images = 0;
        for (auto at = file.find("/Subtype /Image"); at != std::string::npos; at = file.find("/Subtype /Image", at + 1)) ++images;
        CHECK_EQ(images, std::size_t{3}); // the JPEG, the PNG (inside its form) and the PNG's alpha
        CHECK(contains(file, "/SMask"));
        CHECK(contains(file, "/DCTDecode")); // the JPEG as it is
        CHECK(!objects_inside_streams(file));
    }

    SECTION("Links within the file, bookmarks and the document's information");
    {
        const auto path = directory / "outline.pdf";
        {
            sc::pdflite::writer pdf{path};
            pdf.metadata("Title", "Quarterly report");
            pdf.metadata("Author", "Zoë");
            pdf.metadata("Department", "Sales");
            pdf.add_outline("Summary", 1);
            pdf.add_outline("Figures", 2, 100);
            pdf.add_outline("By region", 2, 400, 1);
            pdf.add_outline("Appendix", 3);
            pdf.add_link(3, {50, 50, 100, 20}); // to a page not there yet
            pdf.set_page(3);
            pdf.add_link(1, {50, 50, 100, 20}, 200);
            CHECK_THROWS_AS(pdf.add_link(0, {0, 0, 1, 1}), std::out_of_range);
        }
        const auto file = read(path);
        CHECK(contains(file, "/Type /Outlines"));
        CHECK(contains(file, "/PageMode /UseOutlines"));
        CHECK(contains(file, "/Title (Summary)"));
        CHECK(contains(file, "/Title (By region)"));
        CHECK(contains(file, "/Subtype /Link"));
        CHECK(contains(file, "/XYZ 0 641.88")); // 200 points down an A4 page, 841.89 points high
        CHECK(contains(file, "/Title (Quarterly report)"));
        CHECK(contains(file, "/Department (Sales)"));
        CHECK(!objects_inside_streams(file));

        sc::pdflite::writer missing{directory / "missing.pdf"};
        missing.add_link(5, {0, 0, 10, 10});
        CHECK_THROWS_AS(missing.close(), std::out_of_range);
    }

    SECTION("Passwords");
    {
        const auto path = directory / "locked.pdf";
        {
            sc::pdflite::writer pdf{path, sc::graphics::page_size::A4, sc::graphics::orientation::portrait, {.user_password = "open", .owner_password = "own"}};
            pdf.draw(sc::graphics::text{"secret", {50, 80}});
        }
        const auto file = read(path);
        CHECK(contains(file, "/Encrypt"));
        CHECK(contains(file, "/CFM /AESV3")); // AES-256
        CHECK(!contains(file, "(secret)"));
    }

    SECTION("Built-in fonts: nothing embedded, Adobe's widths");
    {
        const auto path = directory / "builtin.pdf";
        const auto other = directory / "builtin-omega.pdf";
        double width = 0;
        {
            sc::pdflite::writer pdf{path};
            pdf.builtin_fonts(true);
            CHECK(pdf.builtin_fonts());
            const sc::graphics::text hello{"Hello, café €", {50, 80}, {"Helvetica", 20}};
            pdf.draw(hello);
            pdf.draw(sc::graphics::text{"Bold Times", {50, 120}, {"Times New Roman", 14, 700}});
            width = pdf.text_width(sc::graphics::text{"Wai", {}, {"Helvetica", 10}});
        }
        CHECK_EQ(width, (944 + 556 + 222) * 10 / 1000.0); // Adobe's widths, whatever is installed
        const auto file = read(path);
        CHECK(contains(file, "/BaseFont /Helvetica"));
        CHECK(contains(file, "/BaseFont /Times-Bold"));
        CHECK(contains(file, "/Encoding /WinAnsiEncoding"));
        CHECK(!contains(file, "/FontFile")); // nothing embedded
        {
            sc::pdflite::writer pdf{other};
            pdf.builtin_fonts(true);
            pdf.draw(sc::graphics::text{"Ohm: Ω", {50, 80}, {"Helvetica", 20}}); // Ω isn't in Windows-1252
        }
        CHECK(contains(read(other), "/FontFile")); // so that part is embedded
        CHECK(!objects_inside_streams(read(other)));
    }

    SECTION("Text measurements");
    {
        sc::pdflite::writer pdf{directory / "measure.pdf"};
        const double narrow = pdf.text_width(sc::graphics::text{"iii"});
        const double wide = pdf.text_width(sc::graphics::text{"WWW", {}, {"sans-serif", 12}});
        CHECK_LT(0.0, narrow);
        CHECK_LT(narrow, wide);
        CHECK_NEAR(pdf.text_width(sc::graphics::text{"WWW", {300, 400}, {"sans-serif", 24}}), 2 * wide, 0.01); // position doesn't matter
        CHECK_EQ(pdf.text_width(sc::graphics::text{}), 0.0);
        const double x_height = pdf.text_x_height({"sans-serif", 100});
        const double cap_height = pdf.text_cap_height({"sans-serif", 100});
        CHECK_BETWEEN(x_height, 30.0, 70.0);
        CHECK_LT(x_height, cap_height);
        CHECK_BETWEEN(cap_height, 50.0, 90.0);
    }

    SECTION("Moving a pdf");
    {
        const auto path = directory / "moved.pdf";
        {
            sc::pdflite::writer first{path};
            sc::pdflite::writer second{std::move(first)};
            second.draw(sc::graphics::text{"Moved", {72, 72}});
        }
        CHECK(read(path).starts_with("%PDF-1.7"));
    }

    SECTION("Mistakes");
    {
        CHECK_THROWS_AS(sc::pdflite::writer(directory / "no-such-directory" / "x.pdf"), std::runtime_error);
        sc::pdflite::writer pdf{directory / "mistakes.pdf"};
        CHECK_THROWS_AS(pdf.draw_image(resources / "no-such-image.png", {0, 0, 100, 100}), std::runtime_error);
    }

    std::filesystem::remove_all(directory);
    TEST_SUMMARY();
}
