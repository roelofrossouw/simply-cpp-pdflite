#ifndef SC_PDFLITE_JPEG_H
#define SC_PDFLITE_JPEG_H

#include "image.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace sc::pdflite {
    // A JPEG image (ISO 32000-1, 8.9), written into the file as it is (DCTDecode, 7.4.8): nothing
    // is decoded or recompressed. Copies share the bytes, and a document writes an image once
    // however often it's drawn. Its header is checked when it's made, throwing
    // std::invalid_argument for what PDF readers needn't read: a JPEG that isn't baseline or
    // progressive (lossless, hierarchical or arithmetic-coded), one with 12-bit samples, or one
    // whose height comes later (a DNL marker). Grey, RGB and CMYK JPEGs are taken; an Adobe CMYK
    // one (its APP14 marker) is written with the Decode array that undoes Adobe's inversion. An
    // EXIF orientation isn't applied: draw it turned.
    class jpeg : public image {
    public:
        // A JPEG file; throws std::runtime_error when it can't be read.
        [[nodiscard]] static jpeg load(const std::filesystem::path &path);

        // A JPEG's bytes.
        explicit jpeg(std::string bytes);

        // In pixels.
        [[nodiscard]] sc::size size() const override { return {width_, height_}; }

        // 1 (grey), 3 (RGB) or 4 (CMYK).
        [[nodiscard]] int components() const { return components_; }

        [[nodiscard]] const std::string &bytes() const { return *bytes_; }

        [[nodiscard]] std::string_view prefix() const override { return "Im"; }

        void write(file &out, std::uint32_t id) const override;

        [[nodiscard]] std::shared_ptr<const image> clone() const override { return std::make_shared<jpeg>(*this); }

        [[nodiscard]] const void *identity() const override { return bytes_.get(); }

    private:
        std::shared_ptr<const std::string> bytes_;
        int width_ = 0, height_ = 0, components_ = 0;
        bool adobe_ = false; // an Adobe APP14 marker: CMYK written inverted
    };
}

#endif //SC_PDFLITE_JPEG_H
