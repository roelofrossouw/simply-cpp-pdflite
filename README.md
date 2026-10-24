# simply-cpp-pdflite

simply-cpp's own PDF writer, in C++20: strict ISO 32000-1 (PDF 1.7), with no library but zlib.
It's the engine under [sc-pdf](https://github.com/roelofrossouw/simply-cpp-pdf), whose
`sc::pdf` canvas draws through it; use sc-pdflite directly for PDFs object by object.

What it writes so far:

- **The file:** a PDF 1.7 header, objects written as they're finished, a cross-reference table
  of exact 20-byte entries, and a trailer with the file's identifier.
- **Pages** of one size, each with a content stream compressed with zlib; an earlier page can be
  drawn on again until the document is closed. Each page has its own size, the document's unless
  `add_page()` gives one: one of sc-graphics' paper sizes
  (`sc::page_size::A4`, A3, A5, Letter, Legal, and `sc::orientation`, as sc-pdf and sc-mupdf
  take them) or any width and height in points.
- **Content:** paths (lines, curves, rectangles), RGB fill and stroke colours, line width,
  transforms, clipping (nonzero and even-odd), graphics states and text, and transparency
  (`document::graphics_state(fill_alpha, stroke_alpha)`, an ExtGState). The content stream
  refuses what PDF forbids, throwing `std::logic_error`: an unbalanced `q`/`Q`, a path inside a
  text object, text without a font, anything between `W` and the path's painting operator.
- **PDF's built-in fonts:** the twelve Latin standard fonts (Helvetica, Times and Courier, with
  their bold and italic styles), named by sc-graphics' `sc::standard_font::face` enum (a typo
  doesn't compile), in WinAnsiEncoding; `document::win_ansi()` turns UTF-8 into it.
  Their metrics are sc-graphics' `sc::standard_font`, so text is measured as readers draw it.
- **The document's information:** title, author and the like, with Unicode, plus the creation
  date and producer.
- **JPEG images** (`pdflite/jpeg.h`, an `image` from `pdflite/image.h`), written into the file as they are and once however often
  they're drawn: grey, RGB or CMYK (Adobe's inverted CMYK corrected), baseline or progressive;
  a JPEG PDF readers needn't read (lossless, arithmetic-coded, 12-bit) is refused. They're drawn
  into a `sc::rect`, or through any `sc::transform` of their unit square: mirrored, turned, scaled.
- **PNG images** (`pdflite/png.h`): one without transparency that isn't interlaced (grey, RGB or a
  palette, at any bit depth) is copied as it is, with PNG's own predictors; any other is decoded
  with zlib (interlacing undone) and its transparency (an alpha channel, a palette's alphas or a
  key colour) written as a soft mask. Each chunk's CRC and order are checked.
- **Each picture written once**: copies of one, and pictures read from the same bytes (a logo loaded
  for every page), share one image. `document::image_resolution(150)` writes PNGs at most about
  150 pixels an inch where they're shown (given with `image(picture, shown)`): one with more than
  1.5 times that is scaled down (`png::reduced()`, an area average), as Ghostscript does.
- **SVG drawings** (`pdflite/svg.h`), read by sc-graphics (`sc::drawing`) and written as a form:
  vectors, sharp at any size, placed like a picture. Strokes, fills (nonzero or even-odd), clips,
  transparency, text in the built-in fonts and the PNGs or JPEGs in it.
- **Embedded fonts** (`pdflite/font.h`): any font FreeType reads, found as an `sc::font` asks
  (fontconfig) or from a file, embedded as a subset of the glyphs used: a TrueType program made
  from FreeType's outlines (a variable font's at the instance asked for, a CFF font's curves turned
  quadratic), with a ToUnicode map so the text can be copied and searched, and the font's own
  widths. Measured with `width()`, `ascender()`, `descender()`, `cap_height()`, `x_height()`.
- **Passwords and permissions** (`pdflite/encryption.h`, `document::protect()`): AES-256
  (revision 6), every string and stream encrypted, a user password to open the file (or none)
  and an owner password for every right; printing, copying, changing, commenting, forms and
  assembling allowed or not. The cryptography is OpenSSL's libcrypto. In a PDF 1.7 file it's
  written with Adobe's extension level 8, as Acrobat writes it.
- **Links:** an area of a page that opens a URI (written in 7-bit ASCII, percent-encoded), or
  goes to a page and a height on it, which needn't exist yet.
- **Bookmarks:** the reader's outline, nested by level, with Unicode titles; the file opens with
  it showing.

And what it reads (since 4.3.0):

- **Existing PDFs** (`pdflite/reader.h`, `sc::pdflite::reader`): their objects and pages, from
  cross-reference tables or streams, object streams and incremental updates (the newest object
  wins), FlateDecode with its predictors decoded. Files encrypted by the standard security handler
  are opened with the user's or the owner's password (or none, when only an owner's is set): RC4
  40 and 128 bits, AES-128 (revisions 2 to 4) and AES-256 (revision 6).
- **Damaged files repaired** as they're read, as qpdf and MuPDF repair them: when the
  cross-reference doesn't lead to the objects (a file cut short, offsets that are off, a
  `startxref` pointing nowhere, a table that can't be read), it's rebuilt by scanning the file for
  them, objects in object streams included, and the trailer pieced together from what's left of
  it (the catalog found by its `/Type` when no trailer names it); `reader::repaired()` says so. A
  page tree that can't be read gives the pages in the order they're in the file. A stream whose
  `/Length` is wrong is read up to its `endstream`. What can't be repaired throws
  `std::runtime_error`: no catalog or pages left, or an RC4 or AES-128 file whose trailer (and
  with it the file's `/ID`, which their key is made from) is lost.
- **Merging** (`pdflite/merge.h`, `sc::pdflite::merge()`): the pages of several PDFs, in order, in
  a new file, which may be one of the inputs. Each page comes as it was (content, resources,
  annotations, links to pages of its own file); the first file's title, bookmarks and named
  destinations come too. Streams are copied as they're encoded, so nothing is decompressed and
  compressed again.
- **Pictures as pages** (since 4.4.0): a JPEG or PNG among `merge()`'s files (known by its first
  bytes, not its name) becomes a page the size of the page before it (of the next file's first
  page when it comes first; A4 without any PDF), turned upright as its EXIF orientation says (a
  phone photo comes out the right way up), scaled to fit keeping its proportions, and centred. Its
  data goes in as it is (a JPEG isn't recompressed). Other formats (GIF, TIFF, HEIC ...) aren't
  read yet: convert them first.
- **Passwords for an existing file** (`sc::pdflite::protect()`, or `merge()` with a
  `protection`): AES-256, whatever the input had.
- **Backgrounds and stamps** (`sc::pdflite::decorate()`): another PDF's pages drawn behind a file's
  pages (a letterhead) or over them (a stamp), its page n on page n and its last on the rest, as
  pdftk's `multibackground` and `multistamp` do (and drawn exactly as pdftk draws them): turned
  upright as each page is shown, scaled to fit the page's crop box and centred. Each decoration
  page is a form XObject, written once however many pages show it.

Still to come: a font chosen per character when one font lacks it, and an `sc::canvas` on top.
Values are checked as they're written: a number that isn't finite, a page side readers can't show
(outside 3 to 14,400 points) or malformed UTF-8 throws `std::invalid_argument`.

Positions are the same as on sc-graphics' canvas: points (1/72 inch) from the page's top-left
corner, y down, and areas are sc-core's `sc::rect`. Positions and sizes are `sc::distance`:
`sc::dist::mm(10)`, `sc::dist::inch(1)` or a plain number of points, mixed as you like. They're turned into PDF's own (y up from
the bottom) as they're written, transforms included.

Each part has its own header (and source), to work on and include alone: `pdflite/object.h`
(PDF's values: `name`, `string`, `reference`, `array`, `dictionary`, and `object` holding any of
them, each writing itself and checked as it's made), `pdflite/file.h` (the file: objects, the cross-reference table,
the trailer), `pdflite/content.h` (a page's content stream), `pdflite/document.h` (pages,
fonts, information), `pdflite/reader.h` (an existing file read) and `pdflite/merge.h` (`merge()`,
`protect()`). `pdflite.h` includes them all.

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
SC_SHOW(pdf.size());          // distances, in points (1/72 inch)
SC_SHOW(pdf.size().in<mm>()); // the same, in millimetres

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
page.text_position((pdf.size().width() - width) / 2, 82); // centred, the baseline 82 points from the top
page.show(*sc::pdflite::document::win_ansi(heading));
page.end_text();

const auto serif = pdf.standard_font(sc::standard_font::face::times_roman);
page.begin_text();
page.font(serif, 13);
page.text_position(inch(1), 122); // a unit from sc::dist, or a plain number of points
SC_STEP(page.show(*sc::pdflite::document::win_ansi("Strict PDF 1.7, no library but zlib: caf\xC3\xA9, \xE2\x82\xAC 12, (brackets) and \\.")));
page.end_text();

sc::console::heading("Shapes: a path, then how to paint it");
page.save();
page.fill_colour(0.85, 0.93, 0.98);
page.stroke_colour(0.16, 0.44, 0.59);
page.line_width(2);
SC_STEP(page.rectangle(mm(25.4), 162, mm(77.6), 120)); // left, top, width, height
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
SC_STEP(pdf.info("Author", "Roelof Rossouw"));
SC_STEP(pdf.info("Subject", "Text in the built-in fonts, shapes and clipping, on two pages"));
SC_STEP(pdf.info("Creator", "sc-pdflite-demo"));
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
SC_SHOW(pdf.size().in<mm>()); // the current page's size, in mm
frame_and_size(pdf, "A4 portrait");

sc::console::heading("Page 2: a size of its own, A3 landscape (add_page(paper, orientation))");
SC_STEP(pdf.add_page(sc::page_size::A3, sc::orientation::landscape));
SC_SHOW(pdf.size().in<mm>()); // now page 2's
frame_and_size(pdf, "A3 landscape");

sc::console::heading("Links and bookmarks find each page's own size");
pdf.set_page(1);
const auto font = pdf.standard_font(sc::standard_font::face::helvetica);
auto &first = pdf.page_content();
first.begin_text();
first.font(font, 12);
first.text_position(mm(25), mm(30));
first.show("To the A3 page");
first.end_text();
SC_STEP(pdf.link(sc::rect{mm(25), mm(25), mm(32), mm(7)}, 2));
SC_STEP(pdf.bookmark("A4 portrait", 1));
SC_STEP(pdf.bookmark("A3 landscape", 2));
pdf.info("Title", "sc-pdflite: pages of different sizes");
pdf.info("Author", "Roelof Rossouw");
pdf.info("Subject", "An A4 portrait page and an A3 landscape one in one file");
pdf.info("Creator", "sc-pdflite-pages");
SC_STEP(pdf.close());
SC_SHOW(std::filesystem::file_size(output)); // bytes
```
<!-- /sc-example -->

## Images

`sc-pdflite-images` draws a photo as it is, mirrored, turned and scaled, and a picture drawn with
sc-image and encoded as a JPEG in memory. It's built when sc-image is installed, and isn't
installed itself, so sc-pdflite's runtime package doesn't depend on sc-image and OpenCV. Its code,
copied from `examples/sc-pdflite-images.cpp` at configure time:

<!-- sc-example: examples/sc-pdflite-images.cpp -->
```cpp
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
```
<!-- /sc-example -->

The photo, `examples/resource/cat.jpg`, is "Cat is sitting on the wall" by Vyshnavi medicharla,
from [Wikimedia Commons](https://commons.wikimedia.org/wiki/File:Cat_is_sitting_on_the_wall.jpg),
under [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) (public domain): Commons'
960-pixel-wide copy, unchanged.

## PNG images

`sc-pdflite-png` draws PNGs over stripes, so their transparency shows: an alpha channel, the same
PNG interlaced, a palette with a see-through colour, and an opaque one copied as it is. Its
pictures are drawn by `examples/resource/make-demo-pngs.py` and installed with it. Its code,
copied from `examples/sc-pdflite-png.cpp` at configure time:

<!-- sc-example: examples/sc-pdflite-png.cpp -->
```cpp
sc::pdflite::document pdf{output, sc::page_size::A4};
auto &page = pdf.page_content();
stripes(page, sc::rect{mm(15), mm(15), mm(180), mm(200)});

sc::console::heading("An alpha channel: decoded, its alpha written as a soft mask");
const auto badge = sc::pdflite::png::load(folder / "badge.png");
SC_SHOW(badge.size());
SC_SHOW(badge.colour_type()); // 6: RGB and alpha
SC_SHOW(badge.copied());      // false: an alpha channel has to be split off
const sc::rect left{mm(25), mm(25), mm(70), mm(70)};
SC_STEP(page.draw(pdf.image(badge), left));
caption(pdf, left, "RGBA: the edge fades out");

sc::console::heading("Interlaced (Adam7): the passes put back together");
const auto interlaced = sc::pdflite::png::load(folder / "badge-interlaced.png");
SC_SHOW(interlaced.interlaced());
const sc::rect right{mm(115), mm(25), mm(70), mm(70)};
SC_STEP(page.draw(pdf.image(interlaced), right));
caption(pdf, right, "The same, interlaced");

sc::console::heading("A palette with a see-through colour (tRNS)");
const auto stamp = sc::pdflite::png::load(folder / "stamp.png");
SC_SHOW(stamp.colour_type()); // 3: a palette
SC_SHOW(stamp.transparent()); // its colour 0 is see-through
const sc::rect middle{mm(70), mm(115), mm(70), mm(70)};
SC_STEP(page.draw(pdf.image(stamp), middle));
caption(pdf, middle, "A palette, its first colour see-through");

sc::console::heading("Without transparency, a PNG is copied as it is: nothing decoded");
const auto gradient = sc::pdflite::png::load(folder / "gradient.png");
SC_SHOW(gradient.copied()); // true: its compressed data goes in with PNG's own predictors
const sc::rect below{mm(45), mm(230), mm(120), mm(40)};
SC_STEP(page.draw(pdf.image(gradient), below));
caption(pdf, below, "RGB without transparency: copied as it is");

pdf.info("Title", "sc-pdflite: PNG images");
pdf.info("Author", "Roelof Rossouw");
pdf.info("Subject", "PNG images with an alpha channel, interlaced, and a palette with a see-through colour, over stripes");
pdf.info("Creator", "sc-pdflite-png");
SC_STEP(pdf.close());
SC_SHOW(std::filesystem::file_size(output)); // bytes
```
<!-- /sc-example -->

## SVG drawings

`sc-pdflite-svg` draws a scene from an SVG file (shapes, a transform, transparency, an even-odd
fill, a clip and text) large, small, mirrored and turned, all one form, and a badge from SVG
text. Its drawing, `examples/resource/drawing.svg`, was made for it and is installed with it. Its
code, copied from `examples/sc-pdflite-svg.cpp` at configure time:

<!-- sc-example: examples/sc-pdflite-svg.cpp -->
```cpp
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
```
<!-- /sc-example -->

## Embedded fonts

`sc-pdflite-fonts` writes text in the installed fonts fontconfig finds: a centred title,
other alphabets, four weights of one family and a fixed-width face, each embedded as a subset.
Its code, copied from `examples/sc-pdflite-fonts.cpp` at configure time:

<!-- sc-example: examples/sc-pdflite-fonts.cpp -->
```cpp
sc::pdflite::document pdf{output, sc::page_size::A4};

sc::console::heading("A font as an sc::font asks for it: fontconfig finds it, FreeType reads it");
const sc::pdflite::font title{sc::font::bold("sans-serif", 26)};
SC_SHOW(title.name()); // its PostScript name
const std::string heading = "Embedded fonts, as subsets";
const auto width = title.width(heading, 26);
SC_SHOW(width);
write(pdf, title, 26, (pdf.size().width() - width) / 2, mm(30), heading); // centred

sc::console::heading("Text in it is glyphs: encode() picks them and records them for the subset");
const sc::pdflite::font serif{sc::font{"serif", 13}};
SC_SHOW(serif.name());
write(pdf, serif, 13, mm(20), mm(45), "Only the glyphs this page uses go into the file, and a ToUnicode map lets");
write(pdf, serif, 13, mm(20), mm(51), "readers copy and search the text: café, naïve, “quotes”, € 12,50.");

sc::console::heading("Other alphabets, where the font has them (covers())");
const sc::pdflite::font sans{sc::font{"sans-serif", 14}};
const std::string greek = "Καλημέρα κόσμε", cyrillic = "Привет, мир";
SC_SHOW(sans.covers(greek));
SC_SHOW(sans.covers(cyrillic));
write(pdf, sans, 14, mm(20), mm(66), "Greek: " + greek);
write(pdf, sans, 14, mm(20), mm(74), "Cyrillic: " + cyrillic);

sc::console::heading("Weights: each the face fontconfig finds for it");
sc::distance y = mm(92);
for (const int weight: {300, 400, 600, 800}) {
    const sc::pdflite::font face{sc::font{"sans-serif", 16, weight}};
    SC_SHOW(face.name());
    write(pdf, face, 16, mm(20), y, "Weight " + std::to_string(weight) + ": " + face.name());
    y = y + mm(9);
}

sc::console::heading("A fixed-width face, measured");
const sc::pdflite::font mono{sc::font{"monospace", 11}};
SC_SHOW(mono.width("iiii", 11) == mono.width("MMMM", 11)); // every glyph as wide
write(pdf, mono, 11, mm(20), mm(135), "for (const auto &glyph: subset) write(glyph);");

pdf.info("Title", "sc-pdflite: embedded fonts");
pdf.info("Author", "Roelof Rossouw");
pdf.info("Subject", "Installed fonts embedded as subsets: sans-serif, serif, monospace, weights and other alphabets");
pdf.info("Creator", "sc-pdflite-fonts");
SC_STEP(pdf.close());
SC_SHOW(std::filesystem::file_size(output)); // bytes: subsets, not whole fonts
```
<!-- /sc-example -->

## Passwords

`sc-pdflite-passwords` writes a file that asks for a password to open and one anyone can open
but only print. Its code, copied from `examples/sc-pdflite-passwords.cpp` at configure time:

<!-- sc-example: examples/sc-pdflite-passwords.cpp -->
```cpp
sc::console::heading("A password to open it: everything in the file is encrypted");
const auto locked = folder / "sc-pdflite-locked.pdf";
sc::pdflite::document pdf{locked, sc::page_size::A5};
page_of_text(pdf, "Opened with the password “open sesame”.");
pdf.info("Title", "sc-pdflite: a password to open");
pdf.info("Author", "Roelof Rossouw");
pdf.info("Subject", "A PDF that asks for a password to open, encrypted with AES-256");
pdf.info("Creator", "sc-pdflite-passwords");
SC_STEP(pdf.protect({"open sesame", "admin"})); // user password, owner password
SC_STEP(pdf.close());
SC_SHOW(locked.string());

sc::console::heading("Anyone may open it, and print; the rest takes the owner's password");
const auto print_only = folder / "sc-pdflite-print-only.pdf";
sc::pdflite::document open{print_only, sc::page_size::A5};
page_of_text(open, "Anyone can open and print this; copying needs “admin”.");
open.info("Title", "sc-pdflite: print only");
open.info("Author", "Roelof Rossouw");
open.info("Subject", "A PDF anyone can open and print; its other rights need the owner's password");
open.info("Creator", "sc-pdflite-passwords");
sc::pdflite::permissions allowed;
allowed.print = true;
allowed.copy = false;
allowed.modify = false;
SC_SHOW(allowed.bits()); // /P: what readers may do
SC_STEP(open.protect({"", "admin", allowed})); // no user password: it opens without one
SC_STEP(open.close());
SC_SHOW(print_only.string());
```
<!-- /sc-example -->

## Merging

`sc-pdflite-merge` writes the pages of PDFs, one file after another, into a new one, with
another PDF's pages behind (`--background`) or over them (`--stamp`) and passwords when asked. Its code, copied from `examples/sc-pdflite-merge.cpp` at configure time:

<!-- sc-example: examples/sc-pdflite-merge.cpp -->
```cpp
std::vector<std::filesystem::path> files;
for (const auto &file: args["inputs"]) files.emplace_back(file.get<std::string>());
std::optional<sc::pdflite::protection> protection;
const auto user = args.as<std::string>("user-password"), owner = args.as<std::string>("owner-password");
if (!user.empty() || !owner.empty()) protection = sc::pdflite::protection{user, owner};
const std::filesystem::path output = args.as<std::string>("output");
// The files merged, pictures made pages; or, with a password, each PDF opened with it.
auto merge = [&](const std::optional<sc::pdflite::protection> &with) {
    const auto password = args.as<std::string>("password");
    if (password.empty()) return sc::pdflite::merge(files, output, with);
    std::vector<sc::pdflite::reader> opened;
    for (const auto &file: files) opened.emplace_back(file, password);
    sc::pdflite::merge(std::vector<std::reference_wrapper<const sc::pdflite::reader>>(opened.begin(), opened.end()), output, with);
};
const auto background = args.as<std::string>("background"), stamp = args.as<std::string>("stamp");
if (background.empty() && stamp.empty()) {
    merge(protection);
} else {
    // Merged first, then decorated in place: the passwords go on last.
    merge(std::nullopt);
    if (!background.empty())
        sc::pdflite::decorate(output, background, output, sc::pdflite::layer::behind, stamp.empty() ? protection : std::nullopt);
    if (!stamp.empty()) sc::pdflite::decorate(output, stamp, output, sc::pdflite::layer::over, protection);
}
std::cout << output.string() << ": " << sc::pdflite::reader{output, user.empty() ? owner : user}.page_count() << " pages"
          << (protection ? " (encrypted)" : "") << '\n';
```
<!-- /sc-example -->

Files on disk merge in one call, and a file gets passwords with another:

```cpp
sc::pdflite::merge({"report.pdf", "appendix.pdf"}, "report.pdf");          // into the first one
sc::pdflite::merge({"report.pdf", "photo.jpg", "scan.png"}, "report.pdf");  // pictures as pages
sc::pdflite::protect("report.pdf", "report.pdf", {"", "owner"});            // print only, for anyone
sc::pdflite::decorate("report.pdf", "letterhead.pdf", "report.pdf");       // its page behind every page
sc::pdflite::decorate("report.pdf", "copy.pdf", "report.pdf", sc::pdflite::layer::over); // a stamp
const sc::pdflite::reader pdf{"locked.pdf", "open sesame"};                 // a password to open it
pdf.page_count(); pdf.page(0); pdf.trailer(); pdf.get(12); pdf.stream(12); // pages and objects
```

What isn't copied from the files after the first: their bookmarks, named destinations, form
fields (their widgets still show), tagged structure and document-level metadata. Its test reads
files written by others too (`tests/resource/pdf`: qpdf's cross-reference streams, object
streams, linearized output and every kind of encryption, macOS's Quartz, MuPDF).

## Checking the files

Its tests check the files they write byte by byte (every cross-reference entry points at its
object, `/Size` and `startxref` are right, the streams inflate), and with every PDF checker
that's installed: `check-qpdf` (`qpdf --check`, warnings fail), `check-pdfcpu` (`pdfcpu validate
-m strict`) and `check-mutool` (mutool draws every page, warning-free). A checker that isn't
installed is left out; an encrypted test file's password is in `<file>.password` beside it. On macOS: `brew install qpdf pdfcpu mupdf`.

## Building locally

Run `bash scripts/install.sh` to configure, build, test, package and install.
It needs sc-core, sc-graphics (installed if missing), zlib, FreeType and OpenSSL.

## Usage

```cmake
find_package(sc-pdflite CONFIG REQUIRED)

target_link_libraries(myapp PRIVATE sc::sc-pdflite-shared)
```

```bash
sudo curl -fsSL https://apt.roelof.co.za/setup.sh | bash
sudo apt -y install simply-cpp-pdflite-dev
```
