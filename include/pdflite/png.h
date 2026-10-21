#ifndef SC_PDFLITE_PNG_H
#define SC_PDFLITE_PNG_H

#include "image.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace sc::pdflite {
    // A PNG image (ISO 32000-1, 8.9; PNG: ISO/IEC 15948). One without transparency that isn't
    // interlaced (grey, RGB or a palette, at any bit depth) is written as it is: its compressed
    // data with PNG's own predictors (FlateDecode with /Predictor 15), nothing decoded. Any other
    // is decoded with zlib: interlacing undone, colours written at 8 bits, and its transparency (an
    // alpha channel, a palette's alpha, or a key colour) as a soft mask (/SMask, 11.6.5.3). It's
    // checked when it's made, throwing std::invalid_argument: the signature, every chunk's CRC, the
    // chunks' order, IHDR's values, and no critical chunk PNG doesn't define. Colour profiles and
    // gamma (iCCP, gAMA, ...) aren't applied.
    class png : public image {
    public:
        // A PNG file; throws std::runtime_error when it can't be read.
        [[nodiscard]] static png load(const std::filesystem::path &path);

        // A PNG's bytes.
        explicit png(std::string bytes);

        // In pixels.
        [[nodiscard]] sc::size size() const override;

        // IHDR's: 1, 2, 4, 8 or 16 bits per sample; colour type 0 (grey), 2 (RGB), 3 (palette),
        // 4 (grey and alpha) or 6 (RGB and alpha).
        [[nodiscard]] int bit_depth() const;

        [[nodiscard]] int colour_type() const;

        [[nodiscard]] bool interlaced() const;

        // Whether some pixels are see-through: an alpha channel, or a tRNS chunk.
        [[nodiscard]] bool transparent() const;

        // Whether write() copies its compressed data as it is (no transparency, not interlaced).
        [[nodiscard]] bool copied() const { return !transparent() && !interlaced(); }

        [[nodiscard]] std::string_view prefix() const override { return "Im"; }

        void write(file &out, std::uint32_t id) const override;

        [[nodiscard]] std::shared_ptr<const image> clone() const override { return std::make_shared<png>(*this); }

        [[nodiscard]] const void *identity() const override { return data_.get(); }

    private:
        struct data;
        std::shared_ptr<const data> data_;
    };
}

#endif //SC_PDFLITE_PNG_H
