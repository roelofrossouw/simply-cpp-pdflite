#ifndef SC_PDFLITE_IMAGE_H
#define SC_PDFLITE_IMAGE_H

#include "resource.h"

#include <size.h>

#include <memory>
#include <string_view>

namespace sc::pdflite {
    // Something content::draw() places through a page's unit square, top-left (0, 0) to
    // bottom-right (1, 1) (ISO 32000-1, 8.9 and 8.10): a JPEG, a PNG or an SVG drawing (jpeg.h,
    // png.h, svg.h). A document writes each once however often it's drawn: copies of one count
    // as the same (they share their data), and so do two read from the same bytes.
    class image : public resource {
    public:
        // Its natural size: pixels for a bitmap, the drawing's own units for an SVG. Draw it into a
        // box of these proportions for an undistorted picture.
        [[nodiscard]] virtual sc::size size() const = 0;

        // A copy, for a document to keep.
        [[nodiscard]] virtual std::shared_ptr<const image> clone() const = 0;

        // What copies share, telling the document when it has written one already.
        [[nodiscard]] virtual const void *identity() const = 0;

        // Its file's bytes (a JPEG's or a PNG's), by which a document knows two pictures that
        // aren't copies as the same; empty for one without them (an SVG drawing).
        [[nodiscard]] virtual std::string_view encoded() const { return {}; }

        // It scaled down to pixels, for a document writing it at a lower resolution
        // (document::image_resolution()); nullptr when it can't be (a JPEG, an SVG).
        [[nodiscard]] virtual std::shared_ptr<const image> reduced(const sc::size &pixels) const { return nullptr; }
    };
}

#endif //SC_PDFLITE_IMAGE_H
