#ifndef SC_PDFLITE_H
#define SC_PDFLITE_H

// simply-cpp's own PDF writer: ISO 32000-1 (PDF 1.7), strictly, with zlib, FreeType and OpenSSL
// as its only libraries. Pages with content streams (paths, colours, clipping, transparency,
// text), PDF's built-in fonts and embedded ones, JPEG, PNG and SVG pictures, links, bookmarks and
// encryption; and existing PDFs read, merged and given passwords. Each part has its header, to
// include alone:
//   pdflite/object.h    how values are written: numbers, names, strings, dates, references
//   pdflite/file.h      the file: header, objects, cross-reference table, trailer
//   pdflite/content.h   a page's content stream, which refuses what PDF forbids
//   pdflite/resource.h  what pages use by name, written once: the base of image and the rest
//   pdflite/image.h     pictures, drawn with content::draw(): the base of jpeg (pdflite/jpeg.h),
//                       png (pdflite/png.h) and svg (pdflite/svg.h, vector drawings as forms)
//   pdflite/font.h      embedded fonts (FreeType), written as subsets
//   pdflite/encryption.h  passwords and permissions: AES-256 (OpenSSL)
//   pdflite/document.h  pages, fonts and the document's information
//   pdflite/reader.h    an existing PDF's objects and pages
//   pdflite/merge.h     existing PDFs' pages in a new file (merge), a file given passwords
//                       (protect), another file's pages behind or over its pages (decorate)
//   pdflite/writer.h    a whole PDF drawn as an sc::canvas, as an sc::image is drawn
// Positions are sc-graphics' canvas's: points from the page's top-left corner, y down (PDF's
// own, y up from the bottom, are what gets written).
#include <sc/pdflite/object.h>
#include <sc/pdflite/file.h>
#include <sc/pdflite/content.h>
#include <sc/pdflite/resource.h>
#include <sc/pdflite/image.h>
#include <sc/pdflite/jpeg.h>
#include <sc/pdflite/png.h>
#include <sc/pdflite/svg.h>
#include <sc/pdflite/font.h>
#include <sc/pdflite/encryption.h>
#include <sc/pdflite/document.h>
#include <sc/pdflite/reader.h>
#include <sc/pdflite/merge.h>
#include <sc/pdflite/writer.h>

#endif //SC_PDFLITE_H
