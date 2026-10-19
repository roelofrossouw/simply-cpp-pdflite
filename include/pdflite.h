#ifndef SC_PDFLITE_H
#define SC_PDFLITE_H

// simply-cpp's own PDF writer: ISO 32000-1 (PDF 1.7), strictly, with no library but zlib. Bare
// bones so far: pages with content streams (paths, colours, clipping, text) and PDF's built-in
// fonts. Each part has its header, to include alone:
//   pdflite/object.h    how values are written: numbers, names, strings, dates, references
//   pdflite/file.h      the file: header, objects, cross-reference table, trailer
//   pdflite/content.h   a page's content stream, which refuses what PDF forbids
//   pdflite/resource.h  what pages use by name, written once: the base of image and the rest
//   pdflite/image.h     pictures, drawn with content::draw(): the base of jpeg (pdflite/jpeg.h),
//                       png (pdflite/png.h) and svg (pdflite/svg.h, vector drawings as forms)
//   pdflite/font.h      embedded fonts (FreeType), written as subsets
//   pdflite/encryption.h  passwords and permissions: AES-256 (OpenSSL)
//   pdflite/document.h  pages, fonts and the document's information
// Positions are sc-graphics' canvas's: points from the page's top-left corner, y down (PDF's
// own, y up from the bottom, are what gets written).
#include "pdflite/object.h"
#include "pdflite/file.h"
#include "pdflite/content.h"
#include "pdflite/resource.h"
#include "pdflite/image.h"
#include "pdflite/jpeg.h"
#include "pdflite/png.h"
#include "pdflite/svg.h"
#include "pdflite/font.h"
#include "pdflite/encryption.h"
#include "pdflite/document.h"

#endif //SC_PDFLITE_H
