# simply-cpp-pdflite

simply-cpp's own PDF writer, in C++20: strict ISO 32000-1 (PDF 1.7), with no library but zlib.
It's bare bones for now, for quick PDFs and for growing the writer without touching
[sc-pdf](https://github.com/roelofrossouw/simply-cpp-pdf), which writes with PDFHummus: when
it's grown enough, sc-pdflite can become sc-pdf's engine.

What it writes so far:

- **The file:** a PDF 1.7 header, objects written as they're finished, a cross-reference table
  of exact 20-byte entries, and a trailer with the file's identifier.
- **Pages** of one size, each with a content stream compressed with zlib; an earlier page can be
  drawn on again until the document is closed. Each page has its own size, the document's unless
  `add_page()` gives one: one of sc-graphics' paper sizes
  (`sc::page_size::A4`, A3, A5, Letter, Legal, and `sc::orientation`, as sc-pdf and sc-mupdf
  take them) or any width and height in points.
- **Content:** paths (lines, curves, rectangles), RGB fill and stroke colours, line width,
  transforms, clipping (nonzero and even-odd), graphics states and text. The content stream
  refuses what PDF forbids, throwing `std::logic_error`: an unbalanced `q`/`Q`, a path inside a
  text object, text without a font, anything between `W` and the path's painting operator.
- **PDF's built-in fonts:** the twelve Latin standard fonts (Helvetica, Times and Courier, with
  their bold and italic styles), named by sc-graphics' `sc::standard_font::face` enum (a typo
  doesn't compile), in WinAnsiEncoding; `document::win_ansi()` turns UTF-8 into it.
  Their metrics are sc-graphics' `sc::standard_font`, so text is measured as readers draw it.
- **The document's information:** title, author and the like, with Unicode, plus the creation
  date and producer.
- **Links:** an area of a page that opens a URI (written in 7-bit ASCII, percent-encoded), or
  goes to a page and a height on it, which needn't exist yet.
- **Bookmarks:** the reader's outline, nested by level, with Unicode titles; the file opens with
  it showing.

Still to come: images (JPEG and PNG), then embedded fonts through FreeType.
Values are checked as they're written: a number that isn't finite, a page side readers can't show
(outside 3 to 14,400 points) or malformed UTF-8 throws `std::invalid_argument`.

Positions are the same as on sc-graphics' canvas: points (1/72 inch) from the page's top-left
corner, y down, and areas are sc-core's `sc::rect`. Positions and sizes are `sc::distance`:
`sc::dist::mm(10)`, `sc::dist::inch(1)` or a plain number of points, mixed as you like. They're turned into PDF's own (y up from
the bottom) as they're written, transforms included.

Each part has its own header (and source), to work on and include alone: `pdflite/object.h`
(PDF's values: `name`, `string`, `reference`, `array`, `dictionary`, and `object` holding any of
them, each writing itself and checked as it's made), `pdflite/file.h` (the file: objects, the cross-reference table,
the trailer), `pdflite/content.h` (a page's content stream) and `pdflite/document.h` (pages,
fonts, information). `pdflite.h` includes them all.

**Documentation:** the [simply-cpp wiki](https://github.com/roelofrossouw/simply-cpp/wiki) has a [PDF guide](https://github.com/roelofrossouw/simply-cpp/wiki/PDF), plus [getting started](https://github.com/roelofrossouw/simply-cpp/wiki/Getting-Started) for the whole suite.

## Demos

Each feature gets a demo of its own as it's added, so they read as a short course.
`sc-pdflite-demo` writes two pages: centred text in the built-in fonts, shapes and a clipped
drawing. It's installed with the runtime package (`simply-cpp-pdflite`). Its code, copied from
`examples/sc-pdflite-demo.cpp` at configure time:

<!-- sc-example: examples/sc-pdflite-demo.cpp -->
```cpp
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
```
<!-- /sc-example -->

## Links and bookmarks

`sc-pdflite-links` writes a booklet: a contents page whose lines go to the chapters, a way back
on each chapter, a web link, and bookmarks for the reader's outline. Its code, copied from
`examples/sc-pdflite-links.cpp` at configure time:

<!-- sc-example: examples/sc-pdflite-links.cpp -->
```cpp
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
const auto site = write_line(pdf, sc::standard_font::face::helvetica, 11, 72, pdf.height() - 100, "simply-cpp on GitHub");
SC_STEP(pdf.link(site, "https://github.com/roelofrossouw/simply-cpp"));

sc::console::heading("Chapters, each with a way back to the contents");
for (std::size_t i = 0; i < chapters.size(); ++i) {
    pdf.set_page(i + 2);
    write_line(pdf, sc::standard_font::face::helvetica_bold, 20, 72, top, std::to_string(i + 1) + ". " + chapters[i]);
    write_line(pdf, sc::standard_font::face::times_roman, 12, 72, top + 40, "Section " + std::to_string(i + 1) + ".1");
    write_line(pdf, sc::standard_font::face::times_roman, 12, 72, top + 300, "Section " + std::to_string(i + 1) + ".2");
    const auto back = write_line(pdf, sc::standard_font::face::helvetica, 10, 72, pdf.height() - 60, "Back to the contents");
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
SC_STEP(pdf.close());
SC_SHOW(std::filesystem::file_size(output)); // bytes
```
<!-- /sc-example -->

## Pages of different sizes

`sc-pdflite-pages` writes an A4 portrait page and an A3 landscape one into one file
(`add_page(paper, orientation)`, or any width and height), each framed with its size written in
the middle, and a link from the first to the second. Its code, copied from
`examples/sc-pdflite-pages.cpp` at configure time:

<!-- sc-example: examples/sc-pdflite-pages.cpp -->
```cpp
sc::console::heading("Page 1: the document's size, A4 portrait");
sc::pdflite::document pdf{output, sc::page_size::A4};
SC_SHOW(static_cast<sc::dist::mm>(pdf.width()));  // the current page's size
SC_SHOW(static_cast<sc::dist::mm>(pdf.height()));
frame_and_size(pdf, "A4 portrait");

sc::console::heading("Page 2: a size of its own, A3 landscape (add_page(paper, orientation))");
SC_STEP(pdf.add_page(sc::page_size::A3, sc::orientation::landscape));
SC_SHOW(static_cast<sc::dist::mm>(pdf.width()));  // now page 2's
SC_SHOW(static_cast<sc::dist::mm>(pdf.height()));
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
```
<!-- /sc-example -->

## Checking the files

Its tests check the files they write byte by byte (every cross-reference entry points at its
object, `/Size` and `startxref` are right, the streams inflate), and with every PDF checker
that's installed: `check-qpdf` (`qpdf --check`, warnings fail), `check-pdfcpu` (`pdfcpu validate
-m strict`) and `check-mutool` (mutool draws every page, warning-free). A checker that isn't
installed is left out. On macOS: `brew install qpdf pdfcpu mupdf`.

## Building locally

Run `bash scripts/install.sh` to configure, build, test, package and install.
It needs sc-core, sc-graphics (installed if missing) and zlib.

## Usage

```cmake
find_package(sc-pdflite CONFIG REQUIRED)

target_link_libraries(myapp PRIVATE sc::sc-pdflite-shared)
```

```bash
sudo curl -fsSL https://apt.roelof.co.za/setup.sh | bash
sudo apt -y install simply-cpp-pdflite-dev
```
