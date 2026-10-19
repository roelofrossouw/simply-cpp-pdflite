#include "pdflite/png.h"

#include "pdflite/object.h"

#include <zlib.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace sc::pdflite {
    struct png::data {
        std::uint32_t width = 0, height = 0;
        int depth = 0, type = 0;
        bool interlace = false;
        std::string palette;     // PLTE: RGB triples
        std::string transparency; // tRNS: a palette's alphas, or the key colour
        std::string compressed;  // the IDAT chunks' data, joined
    };

    namespace {
        [[noreturn]] void bad(const std::string &why) { throw std::invalid_argument("sc::pdflite: a PNG " + why); }

        std::uint32_t word(const std::string &bytes, const std::size_t at) {
            if (at + 4 > bytes.size()) bad("that ends too soon");
            return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at])) << 24 |
                   static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 16 |
                   static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 2])) << 8 |
                   static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 3]));
        }

        // Samples per pixel for each colour type.
        int channels(const int type) {
            switch (type) {
                case 0: return 1;
                case 2: return 3;
                case 3: return 1;
                case 4: return 2;
                case 6: return 4;
                default: bad("of an unknown colour type");
            }
        }

        // Adam7's passes (x0, y0, dx, dy), or the one pass of a picture that isn't interlaced.
        constexpr std::array<std::array<int, 4>, 7> adam7{{{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4},
                                                          {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}}};

        std::uint32_t pass_size(const std::uint32_t size, const int start, const int step) {
            return size > static_cast<std::uint32_t>(start) ? (size - start + step - 1) / step : 0;
        }

        int paeth(const int a, const int b, const int c) {
            const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
            return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
        }
    }

    png png::load(const std::filesystem::path &path) {
        std::ifstream in{path, std::ios::binary};
        if (!in) throw std::runtime_error("sc::pdflite: can't read " + path.string());
        return png{std::string{std::istreambuf_iterator<char>{in}, {}}};
    }

    png::png(std::string bytes) {
        if (bytes.compare(0, 8, "\x89PNG\r\n\x1A\n") != 0) bad("without its signature (not a PNG)");
        auto parsed = std::make_shared<data>();
        bool header = false, ended = false;
        enum { before, inside, after } image_data = before; // where the IDAT chunks are: together, after PLTE and tRNS
        for (std::size_t at = 8; !ended;) {
            const std::uint32_t length = word(bytes, at);
            if (length > 0x7FFFFFFF || at + 12 + length > bytes.size()) bad("with a chunk that runs past its end");
            const std::string type = bytes.substr(at + 4, 4);
            const std::string body = bytes.substr(at + 8, length);
            const auto crc = crc32(crc32(0, nullptr, 0), reinterpret_cast<const Bytef *>(bytes.data() + at + 4), length + 4);
            if (crc != word(bytes, at + 8 + length)) bad("whose " + type + " chunk fails its CRC");
            at += 12 + length;
            if (!header && type != "IHDR") bad("that doesn't start with IHDR");
            if (type != "IDAT" && image_data == inside) image_data = after;
            if (type == "IHDR") {
                if (header || length != 13) bad("with a broken IHDR");
                header = true;
                parsed->width = word(body, 0);
                parsed->height = word(body, 4);
                parsed->depth = static_cast<unsigned char>(body[8]);
                parsed->type = static_cast<unsigned char>(body[9]);
                const int compression = static_cast<unsigned char>(body[10]), filter = static_cast<unsigned char>(body[11]),
                          interlace = static_cast<unsigned char>(body[12]);
                if (parsed->width == 0 || parsed->height == 0 || parsed->width > 0x7FFFFFFF || parsed->height > 0x7FFFFFFF) bad("without a size");
                const int d = parsed->depth;
                channels(parsed->type); // throws for an unknown colour type
                const bool depth_ok = parsed->type == 0 ? (d == 1 || d == 2 || d == 4 || d == 8 || d == 16)
                                      : parsed->type == 3 ? (d == 1 || d == 2 || d == 4 || d == 8)
                                      : (d == 8 || d == 16);
                if (!depth_ok) bad("whose bit depth its colour type doesn't allow");
                if (compression != 0 || filter != 0 || interlace > 1) bad("with a compression, filter or interlace method PNG doesn't define");
                parsed->interlace = interlace == 1;
            } else if (type == "PLTE") {
                if (image_data != before || !parsed->palette.empty() || parsed->type == 0 || parsed->type == 4) bad("with a PLTE it can't have");
                const std::uint32_t most = parsed->type == 3 ? 1u << parsed->depth : 256;
                if (length == 0 || length % 3 != 0 || length / 3 > most) bad("with a broken palette");
                parsed->palette = body;
            } else if (type == "tRNS") {
                if (image_data != before) bad("with tRNS after its image data");
                if (parsed->type == 3 ? length > parsed->palette.size() / 3 : parsed->type == 0 ? length != 2 : parsed->type == 2 ? length != 6 : true)
                    bad("with a tRNS its colour type can't have");
                parsed->transparency = body;
            } else if (type == "IDAT") {
                if (image_data == after) bad("whose IDAT chunks aren't together");
                image_data = inside;
                parsed->compressed += body;
            } else if (type == "IEND") {
                ended = true;
            } else if (!(type[0] & 0x20)) {
                bad("with a critical chunk PNG doesn't define (" + type + ")"); // ancillary ones (lower case first) are skipped
            }
        }
        if (parsed->type == 3 && parsed->palette.empty()) bad("with a palette's colour type but no PLTE");
        if (parsed->compressed.empty()) bad("without image data (IDAT)");
        data_ = std::move(parsed);
    }

    sc::size png::size() const { return {data_->width, data_->height}; }

    int png::bit_depth() const { return data_->depth; }

    int png::colour_type() const { return data_->type; }

    bool png::interlaced() const { return data_->interlace; }

    bool png::transparent() const { return data_->type == 4 || data_->type == 6 || !data_->transparency.empty(); }

    void png::write(file &out, const std::uint32_t id) const {
        const data &d = *data_;
        dictionary entries{{"Type", name{"XObject"}}, {"Subtype", name{"Image"}}, {"Width", d.width}, {"Height", d.height}};

        if (copied()) {
            // The data as it is: FlateDecode, undoing PNG's per-row filters with predictor 15.
            const int colours = d.type == 2 ? 3 : 1;
            // A palette: the colours' indices into it (8.6.6.3).
            if (d.type == 3) entries.add("ColorSpace", array{name{"Indexed"}, name{"DeviceRGB"}, d.palette.size() / 3 - 1, string{d.palette, true}});
            else entries.add("ColorSpace", name{d.type == 2 ? "DeviceRGB" : "DeviceGray"});
            entries.add("BitsPerComponent", d.depth).add("Filter", name{"FlateDecode"});
            entries.add("DecodeParms", dictionary{{"Predictor", 15}, {"Colors", colours}, {"BitsPerComponent", d.depth}, {"Columns", d.width}});
            out.write_stream(id, entries, d.compressed, false);
            return;
        }

        // Decoded: the rows of each pass, unfiltered, then every pixel's colour and alpha at 8 bits.
        const int samples = channels(d.type);
        const int bits_per_pixel = samples * d.depth;
        const std::size_t step = std::max(1, bits_per_pixel / 8); // the filters' "bpp"
        const auto passes = d.interlace ? adam7.size() : std::size_t{1};
        std::size_t raw_size = 0;
        for (std::size_t p = 0; p < passes; ++p) {
            const auto [x0, y0, dx, dy] = d.interlace ? adam7[p] : std::array<int, 4>{0, 0, 1, 1};
            const auto w = pass_size(d.width, x0, dx), h = pass_size(d.height, y0, dy);
            if (w && h) raw_size += h * (1 + (static_cast<std::size_t>(w) * bits_per_pixel + 7) / 8);
        }
        std::string raw(raw_size, '\0');
        uLongf inflated = raw_size;
        if (uncompress(reinterpret_cast<Bytef *>(raw.data()), &inflated, reinterpret_cast<const Bytef *>(d.compressed.data()),
                       d.compressed.size()) != Z_OK || inflated != raw_size)
            bad("whose image data doesn't inflate to its size");

        const bool colour = d.type == 2 || d.type == 3 || d.type == 6;
        const std::size_t pixels = static_cast<std::size_t>(d.width) * d.height;
        std::string colours(pixels * (colour ? 3 : 1), '\0'), alphas(pixels, '\xFF');
        const int top = (1 << d.depth) - 1;
        std::size_t at = 0;
        std::vector<unsigned char> previous, row;
        for (std::size_t p = 0; p < passes; ++p) {
            const auto [x0, y0, dx, dy] = d.interlace ? adam7[p] : std::array<int, 4>{0, 0, 1, 1};
            const auto w = pass_size(d.width, x0, dx), h = pass_size(d.height, y0, dy);
            if (!w || !h) continue;
            const std::size_t length = (static_cast<std::size_t>(w) * bits_per_pixel + 7) / 8;
            previous.assign(length, 0);
            for (std::uint32_t y = 0; y < h; ++y) {
                const int filter = static_cast<unsigned char>(raw[at]);
                row.assign(raw.begin() + static_cast<std::ptrdiff_t>(at + 1), raw.begin() + static_cast<std::ptrdiff_t>(at + 1 + length));
                at += 1 + length;
                for (std::size_t i = 0; i < length; ++i) {
                    const int left = i >= step ? row[i - step] : 0, up = previous[i], corner = i >= step ? previous[i - step] : 0;
                    switch (filter) {
                        case 0: break;
                        case 1: row[i] = static_cast<unsigned char>(row[i] + left); break;
                        case 2: row[i] = static_cast<unsigned char>(row[i] + up); break;
                        case 3: row[i] = static_cast<unsigned char>(row[i] + (left + up) / 2); break;
                        case 4: row[i] = static_cast<unsigned char>(row[i] + paeth(left, up, corner)); break;
                        default: bad("with a row filter PNG doesn't define");
                    }
                }
                // A sample at its depth: whole bytes (16 bits big-endian), or bits from the left.
                const auto sample = [&](const std::size_t index) -> int {
                    if (d.depth == 16) return row[index * 2] << 8 | row[index * 2 + 1];
                    if (d.depth == 8) return row[index];
                    const std::size_t bit = index * d.depth;
                    return (row[bit / 8] >> (8 - d.depth - bit % 8)) & top;
                };
                const auto eight = [&](const int value) { return static_cast<char>(d.depth == 16 ? value >> 8 : value * 255 / top); };
                for (std::uint32_t x = 0; x < w; ++x) {
                    const std::size_t pixel = (static_cast<std::size_t>(y0) + y * dy) * d.width + x0 + x * dx;
                    const std::size_t first = static_cast<std::size_t>(x) * samples;
                    if (d.type == 3) {
                        const auto index = static_cast<std::size_t>(sample(first));
                        if (index * 3 + 2 >= d.palette.size()) bad("with a pixel beyond its palette");
                        for (int c = 0; c < 3; ++c) colours[pixel * 3 + c] = d.palette[index * 3 + c];
                        if (index < d.transparency.size()) alphas[pixel] = d.transparency[index];
                    } else if (d.type == 0 || d.type == 4) {
                        const int grey = sample(first);
                        colours[pixel] = eight(grey);
                        if (d.type == 4) alphas[pixel] = eight(sample(first + 1));
                        else if (!d.transparency.empty() && grey == (static_cast<unsigned char>(d.transparency[0]) << 8 | static_cast<unsigned char>(d.transparency[1])))
                            alphas[pixel] = 0;
                    } else {
                        bool key = !d.transparency.empty();
                        for (int c = 0; c < 3; ++c) {
                            const int value = sample(first + c);
                            colours[pixel * 3 + c] = eight(value);
                            if (key) key = value == (static_cast<unsigned char>(d.transparency[c * 2]) << 8 | static_cast<unsigned char>(d.transparency[c * 2 + 1]));
                        }
                        if (d.type == 6) alphas[pixel] = eight(sample(first + 3));
                        else if (key) alphas[pixel] = 0;
                    }
                }
                previous = row;
            }
        }

        entries.add("ColorSpace", name{colour ? "DeviceRGB" : "DeviceGray"}).add("BitsPerComponent", 8);
        if (transparent()) {
            // The alpha as a soft mask: a grey image of its own (11.6.5.3).
            const std::uint32_t mask = out.allocate();
            out.write_stream(mask, dictionary{{"Type", name{"XObject"}}, {"Subtype", name{"Image"}}, {"Width", d.width},
                                              {"Height", d.height}, {"ColorSpace", name{"DeviceGray"}}, {"BitsPerComponent", 8}},
                             alphas);
            entries.add("SMask", reference{mask});
        }
        out.write_stream(id, entries, colours);
    }
}
