#ifndef SC_PDFLITE_IMAGE_H
#define SC_PDFLITE_IMAGE_H

#include "resource.h"

#include <memory>

// sc-pdflite's pictures (ISO 32000-1, 8.9 and 8.10): what content::draw() places through its unit
// square. Each kind has its own header: jpeg.h, png.h, svg.h.
namespace sc::pdflite {
    // Something drawn into a page's unit square, top-left (0, 0) to bottom-right (1, 1): a JPEG,
    // a PNG or an SVG drawing. A document writes each once however often it's drawn; copies of one
    // count as the same (they share their data).
    class image : public resource {
    public:
        // Its natural size: pixels for a bitmap, the drawing's own units for an SVG. Draw it into a
        // box of these proportions for an undistorted picture.
        [[nodiscard]] virtual double width() const = 0;

        [[nodiscard]] virtual double height() const = 0;

        // A copy, for a document to keep.
        [[nodiscard]] virtual std::shared_ptr<const image> clone() const = 0;

        // What copies share, telling the document when it has written one already.
        [[nodiscard]] virtual const void *identity() const = 0;
    };
}

#endif //SC_PDFLITE_IMAGE_H
