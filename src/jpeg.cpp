#include "pdflite/jpeg.h"

#include "pdflite/object.h"

#include <fstream>
#include <iterator>
#include <stdexcept>

namespace sc::pdflite {
    namespace {
        int byte_at(const std::string &bytes, const std::size_t at) {
            if (at >= bytes.size()) throw std::invalid_argument("sc::pdflite: a JPEG that ends too soon");
            return static_cast<unsigned char>(bytes[at]);
        }

        int word_at(const std::string &bytes, const std::size_t at) { return byte_at(bytes, at) << 8 | byte_at(bytes, at + 1); }
    }

    jpeg jpeg::load(const std::filesystem::path &path) {
        std::ifstream in{path, std::ios::binary};
        if (!in) throw std::runtime_error("sc::pdflite: can't read " + path.string());
        return jpeg{std::string{std::istreambuf_iterator<char>{in}, {}}};
    }

    jpeg::jpeg(std::string bytes) {
        // The marker segments up to the frame header (ITU T.81, B.1.1): each FF, its marker, and
        // (but for the standalone ones) a two-byte length that counts itself.
        if (byte_at(bytes, 0) != 0xFF || byte_at(bytes, 1) != 0xD8) throw std::invalid_argument("sc::pdflite: not a JPEG (no SOI marker)");
        std::size_t at = 2;
        while (true) {
            if (byte_at(bytes, at) != 0xFF) throw std::invalid_argument("sc::pdflite: a JPEG whose markers are out of step");
            int marker = byte_at(bytes, at + 1);
            while (marker == 0xFF) marker = byte_at(bytes, ++at + 1); // fill bytes
            at += 2;
            if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue; // standalone: TEM, RSTn
            if (marker == 0xD9 || marker == 0xDA) throw std::invalid_argument("sc::pdflite: a JPEG without a frame header (SOF)");
            const int length = word_at(bytes, at);
            if (length < 2) throw std::invalid_argument("sc::pdflite: a JPEG with a broken segment");
            if (marker == 0xEE && length >= 7 && bytes.compare(at + 2, 5, "Adobe") == 0) adobe_ = true;
            if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
                // SOF0 baseline, SOF1 extended sequential and SOF2 progressive are what DCTDecode reads;
                // lossless, hierarchical and arithmetic-coded ones aren't.
                if (marker > 0xC2) throw std::invalid_argument("sc::pdflite: a JPEG that isn't baseline or progressive (DCTDecode can't read it)");
                if (byte_at(bytes, at + 2) != 8) throw std::invalid_argument("sc::pdflite: a JPEG with samples of other than 8 bits");
                height_ = word_at(bytes, at + 3);
                width_ = word_at(bytes, at + 5);
                components_ = byte_at(bytes, at + 7);
                if (height_ == 0) throw std::invalid_argument("sc::pdflite: a JPEG whose height comes later (a DNL marker)");
                if (width_ == 0) throw std::invalid_argument("sc::pdflite: a JPEG without a width");
                if (components_ != 1 && components_ != 3 && components_ != 4)
                    throw std::invalid_argument("sc::pdflite: a JPEG of " + std::to_string(components_) + " components (1, 3 or 4)");
                break;
            }
            at += static_cast<std::size_t>(length);
        }
        bytes_ = std::make_shared<const std::string>(std::move(bytes));
    }

    void jpeg::write(file &out, const std::uint32_t id) const {
        dictionary entries{{"Type", name{"XObject"}}, {"Subtype", name{"Image"}}, {"Width", width_}, {"Height", height_},
                           {"ColorSpace", name{components_ == 1 ? "DeviceGray" : components_ == 3 ? "DeviceRGB" : "DeviceCMYK"}},
                           {"BitsPerComponent", 8}};
        // Adobe writes CMYK JPEGs inverted; the Decode array turns them back (8.9.5.2).
        if (components_ == 4 && adobe_) entries.add("Decode", array{1, 0, 1, 0, 1, 0, 1, 0});
        entries.add("Filter", name{"DCTDecode"});
        out.write_stream(id, entries, *bytes_, false); // already compressed: written as it is
    }
}
