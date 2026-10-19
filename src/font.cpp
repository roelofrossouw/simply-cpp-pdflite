#include "pdflite/font.h"

#include "pdflite/object.h"

#include <font_face.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_TRUETYPE_TABLES_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace sc::pdflite {
    struct font::data {
        FT_Library library = nullptr;
        FT_Face face = nullptr;
        std::string name;
        // The glyphs used, by their new number in the subset (0 is .notdef): the font's own glyph
        // and the character it was used for, for ToUnicode.
        std::mutex lock;
        std::vector<std::pair<FT_UInt, char32_t>> glyphs{{0, 0}};
        std::map<FT_UInt, std::uint16_t> renumbered{{0, 0}};

        ~data() {
            if (face) FT_Done_Face(face);
            if (library) FT_Done_FreeType(library);
        }

        // The subset's number for the font's glyph, adding it the first time.
        std::uint16_t use(const FT_UInt glyph, const char32_t character) {
            if (const auto found = renumbered.find(glyph); found != renumbered.end()) return found->second;
            if (glyphs.size() >= 0xFFFF) throw std::length_error("sc::pdflite: more than 65,535 glyphs of one font");
            const auto number = static_cast<std::uint16_t>(glyphs.size());
            glyphs.emplace_back(glyph, character);
            renumbered.emplace(glyph, number);
            return number;
        }
    };

    namespace {
        // A glyph's outline as TrueType has it: contours of points, each on the curve or a quadratic
        // control point, in font units.
        struct point {
            double x, y;
            bool on;
        };

        // Adds the quadratic curves standing in for the cubic p0 c1 c2 p3 to out (p0 already there):
        // split in halves until a quadratic is within a tenth of a unit of it.
        void cubic_to_quadratics(const point &p0, const point &c1, const point &c2, const point &p3, std::vector<point> &out, const int depth = 0) {
            // The quadratic through the cubic's ends whose control point is where the two tangents'
            // estimates meet; how far it strays is |p3 - 3 c2 + 3 c1 - p0| * sqrt(3) / 36.
            const double ex = p3.x - 3 * c2.x + 3 * c1.x - p0.x, ey = p3.y - 3 * c2.y + 3 * c1.y - p0.y;
            const double error = std::hypot(ex, ey) * std::sqrt(3.0) / 36;
            if (error <= 0.1 || depth >= 8) {
                out.push_back({(3 * (c1.x + c2.x) - p0.x - p3.x) / 4, (3 * (c1.y + c2.y) - p0.y - p3.y) / 4, false});
                out.push_back(p3);
                return;
            }
            const auto mid = [](const point &a, const point &b) { return point{(a.x + b.x) / 2, (a.y + b.y) / 2, true}; };
            const point ab = mid(p0, c1), bc = mid(c1, c2), cd = mid(c2, p3), abc = mid(ab, bc), bcd = mid(bc, cd), middle = mid(abc, bcd);
            cubic_to_quadratics(p0, ab, abc, middle, out, depth + 1);
            cubic_to_quadratics(middle, bcd, cd, p3, out, depth + 1);
        }

        // FreeType's outline as TrueType contours: on-curve and quadratic points as they are,
        // cubic segments turned into quadratics.
        std::vector<std::vector<point>> contours_of(const FT_Outline &outline) {
            std::vector<std::vector<point>> out;
            int start = 0;
            for (int c = 0; c < outline.n_contours; ++c) {
                const int end = outline.contours[c];
                std::vector<point> source;
                for (int i = start; i <= end; ++i) {
                    const auto tag = FT_CURVE_TAG(outline.tags[i]);
                    source.push_back({static_cast<double>(outline.points[i].x), static_cast<double>(outline.points[i].y), tag == FT_CURVE_TAG_ON});
                }
                const bool cubic = std::any_of(outline.tags + start, outline.tags + end + 1, [](const char t) { return FT_CURVE_TAG(t) == FT_CURVE_TAG_CUBIC; });
                start = end + 1;
                if (source.empty()) continue;
                if (!cubic) {
                    out.push_back(std::move(source));
                    continue;
                }
                // A CFF contour: on-curve points with pairs of cubic controls between them. Start at
                // an on-curve point and walk round.
                const auto first = static_cast<std::size_t>(std::find_if(source.begin(), source.end(), [](const point &p) { return p.on; }) - source.begin());
                if (first == source.size()) continue;
                std::vector<point> contour{source[first]};
                const std::size_t n = source.size();
                for (std::size_t k = 1; k <= n;) {
                    const point &next = source[(first + k) % n];
                    if (next.on) {
                        if (k < n) contour.push_back(next);
                        ++k;
                    } else {
                        const point &c1 = next, &c2 = source[(first + k + 1) % n], &p3 = source[(first + k + 2) % n];
                        const point p0 = contour.back();
                        cubic_to_quadratics(p0, c1, c2, p3, contour);
                        k += 3;
                        if (k > n) contour.pop_back(); // came back to the start: it's the first point
                    }
                }
                out.push_back(std::move(contour));
            }
            return out;
        }

        void put16(std::string &out, const int value) {
            out += static_cast<char>(value >> 8 & 0xFF);
            out += static_cast<char>(value & 0xFF);
        }

        void put32(std::string &out, const std::uint32_t value) {
            for (const int shift: {24, 16, 8, 0}) out += static_cast<char>(value >> shift & 0xFF);
        }

        std::uint32_t checksum(const std::string &table) {
            std::uint32_t sum = 0;
            for (std::size_t i = 0; i < table.size(); i += 4) {
                std::uint32_t word = 0;
                for (std::size_t k = 0; k < 4; ++k) word = word << 8 | (i + k < table.size() ? static_cast<unsigned char>(table[i + k]) : 0);
                sum += word;
            }
            return sum;
        }

        std::int16_t coordinate(const double value) {
            const auto rounded = std::lround(value);
            if (rounded < std::numeric_limits<std::int16_t>::min() || rounded > std::numeric_limits<std::int16_t>::max())
                throw std::overflow_error("sc::pdflite: a glyph's point outside TrueType's range");
            return static_cast<std::int16_t>(rounded);
        }

        // A simple glyph (TrueType 'glyf'): its contours' ends, no instructions, a flag and two-byte
        // x and y deltas for each point; empty when it has no contours. Padded to 4 bytes.
        std::string glyph_of(const std::vector<std::vector<point>> &contours, std::array<int, 4> &box) {
            std::string out;
            std::size_t points = 0;
            for (const auto &contour: contours) points += contour.size();
            if (points == 0) return out;
            int x_min = std::numeric_limits<int>::max(), y_min = x_min, x_max = std::numeric_limits<int>::min(), y_max = x_max;
            for (const auto &contour: contours)
                for (const auto &p: contour) {
                    x_min = std::min<int>(x_min, coordinate(p.x));
                    x_max = std::max<int>(x_max, coordinate(p.x));
                    y_min = std::min<int>(y_min, coordinate(p.y));
                    y_max = std::max<int>(y_max, coordinate(p.y));
                }
            box = {x_min, y_min, x_max, y_max};
            put16(out, static_cast<int>(contours.size()));
            for (const int v: box) put16(out, v);
            std::size_t end = 0;
            for (const auto &contour: contours) put16(out, static_cast<int>((end += contour.size()) - 1));
            put16(out, 0); // no instructions
            for (const auto &contour: contours)
                for (const auto &p: contour) out += static_cast<char>(p.on ? 1 : 0); // flags: two-byte deltas throughout
            for (const bool y: {false, true}) {
                int previous = 0;
                for (const auto &contour: contours)
                    for (const auto &p: contour) {
                        const int value = coordinate(y ? p.y : p.x);
                        put16(out, value - previous);
                        previous = value;
                    }
            }
            while (out.size() % 4) out += '\0';
            return out;
        }

        // A font program from its tables (OpenType's table directory, 4-byte aligned, checksums, and
        // head's checkSumAdjustment).
        std::string font_program(std::map<std::string, std::string> tables) {
            const auto count = static_cast<int>(tables.size());
            int power = 1, log = 0;
            while (power * 2 <= count) power *= 2, ++log;
            std::string out;
            put32(out, 0x00010000);
            put16(out, count);
            put16(out, power * 16);
            put16(out, log);
            put16(out, count * 16 - power * 16);
            std::uint32_t offset = 12 + 16 * static_cast<std::uint32_t>(count);
            std::string body;
            std::size_t head_at = 0;
            for (auto &[tag, table]: tables) {
                out += tag;
                put32(out, checksum(table));
                put32(out, offset);
                put32(out, static_cast<std::uint32_t>(table.size()));
                if (tag == "head") head_at = offset;
                body += table;
                while (body.size() % 4) body += '\0';
                offset = 12 + 16 * static_cast<std::uint32_t>(count) + static_cast<std::uint32_t>(body.size());
            }
            out += body;
            const std::uint32_t adjustment = 0xB1B0AFBA - checksum(out);
            for (int k = 0; k < 4; ++k) out[head_at + 8 + k] = static_cast<char>(adjustment >> (24 - 8 * k) & 0xFF);
            return out;
        }

        double units(const FT_Face face, const double value) { return value * 1000 / face->units_per_EM; } // to PDF's thousandths
    }

    font font::load(const std::filesystem::path &file, const long index) {
        font out;
        out.data_ = std::make_shared<data>();
        if (FT_Init_FreeType(&out.data_->library) != 0) throw std::runtime_error("sc::pdflite: FreeType couldn't start");
        if (FT_New_Face(out.data_->library, file.string().c_str(), index, &out.data_->face) != 0)
            throw std::runtime_error("sc::pdflite: FreeType can't read the font " + file.string());
        const FT_Face face = out.data_->face;
        if (!FT_IS_SCALABLE(face) || !FT_IS_SFNT(face)) throw std::runtime_error("sc::pdflite: " + file.string() + " isn't a TrueType or OpenType font");
        const char *postscript = FT_Get_Postscript_Name(face);
        out.data_->name = postscript ? postscript : face->family_name ? face->family_name : "Font";
        return out;
    }

    font::font(const sc::font &spec) {
        const auto face = spec.face();
        if (!face) throw std::runtime_error("sc::pdflite: no installed font for " + spec.family);
        *this = load(face->file(), face->index());
    }

    const std::string &font::name() const { return data_->name; }

    std::string font::encode(const std::string_view utf8) const {
        std::lock_guard guard{data_->lock};
        std::string out;
        for (const char32_t code: code_points(utf8)) {
            const auto number = data_->use(FT_Get_Char_Index(data_->face, code), code);
            out += static_cast<char>(number >> 8);
            out += static_cast<char>(number & 0xFF);
        }
        return out;
    }

    bool font::covers(const std::string_view utf8) const {
        return std::ranges::all_of(code_points(utf8), [&](const char32_t code) { return FT_Get_Char_Index(data_->face, code) != 0; });
    }

    sc::distance font::width(const std::string_view utf8, const sc::distance size) const {
        std::lock_guard guard{data_->lock};
        double advance = 0;
        for (const char32_t code: code_points(utf8)) {
            if (FT_Load_Glyph(data_->face, FT_Get_Char_Index(data_->face, code), FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) == 0)
                advance += static_cast<double>(data_->face->glyph->metrics.horiAdvance);
        }
        return advance * size.points() / data_->face->units_per_EM;
    }

    sc::distance font::ascender(const sc::distance size) const { return data_->face->ascender * size.points() / data_->face->units_per_EM; }

    sc::distance font::descender(const sc::distance size) const { return -data_->face->descender * size.points() / data_->face->units_per_EM; }

    sc::distance font::cap_height(const sc::distance size) const {
        const auto *os2 = static_cast<const TT_OS2 *>(FT_Get_Sfnt_Table(data_->face, FT_SFNT_OS2));
        const double height = os2 && os2->version >= 2 && os2->sCapHeight > 0 ? os2->sCapHeight : data_->face->ascender * 0.7;
        return height * size.points() / data_->face->units_per_EM;
    }

    void font::write(file &out, const std::uint32_t id) const {
        std::lock_guard guard{data_->lock};
        const FT_Face face = data_->face;
        const auto &used = data_->glyphs;

        // The subset: each used glyph's outline, at the face's instance, renumbered in order of use.
        std::string glyf, loca, hmtx;
        put32(loca, 0);
        int max_points = 0, max_contours = 0, advance_max = 0, lsb_min = std::numeric_limits<int>::max(), rsb_min = lsb_min, extent_max = std::numeric_limits<int>::min();
        std::array<int, 4> font_box{std::numeric_limits<int>::max(), std::numeric_limits<int>::max(), std::numeric_limits<int>::min(), std::numeric_limits<int>::min()};
        array widths;
        for (const auto &[glyph, character]: used) {
            if (FT_Load_Glyph(face, glyph, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0)
                throw std::runtime_error("sc::pdflite: FreeType couldn't load a glyph of " + data_->name);
            const auto contours = contours_of(face->glyph->outline);
            std::array<int, 4> box{0, 0, 0, 0};
            glyf += glyph_of(contours, box);
            put32(loca, static_cast<std::uint32_t>(glyf.size()));
            const int advance = static_cast<int>(face->glyph->metrics.horiAdvance);
            const int lsb = box[0];
            put16(hmtx, advance);
            put16(hmtx, lsb);
            widths.push_back(std::round(units(face, advance) * 100) / 100);
            std::size_t points = 0;
            for (const auto &contour: contours) points += contour.size();
            max_points = std::max<int>(max_points, static_cast<int>(points));
            max_contours = std::max<int>(max_contours, static_cast<int>(contours.size()));
            advance_max = std::max(advance_max, advance);
            if (points) {
                lsb_min = std::min(lsb_min, lsb);
                rsb_min = std::min(rsb_min, advance - box[2]);
                extent_max = std::max(extent_max, box[2]);
                font_box = {std::min(font_box[0], box[0]), std::min(font_box[1], box[1]), std::max(font_box[2], box[2]), std::max(font_box[3], box[3])};
            }
        }
        if (font_box[0] > font_box[2]) font_box = {0, 0, 0, 0};
        if (lsb_min == std::numeric_limits<int>::max()) lsb_min = rsb_min = extent_max = 0;
        const auto glyph_count = static_cast<int>(used.size());

        const auto *head_table = static_cast<const TT_Header *>(FT_Get_Sfnt_Table(face, FT_SFNT_HEAD));
        const auto *post_table = static_cast<const TT_Postscript *>(FT_Get_Sfnt_Table(face, FT_SFNT_POST));
        const bool italic = face->style_flags & FT_STYLE_FLAG_ITALIC;
        const bool bold = face->style_flags & FT_STYLE_FLAG_BOLD;

        std::string head;
        put32(head, 0x00010000);
        put32(head, head_table ? static_cast<std::uint32_t>(head_table->Font_Revision) : 0x00010000);
        put32(head, 0);          // checkSumAdjustment, set by font_program()
        put32(head, 0x5F0F3CF5); // magic
        put16(head, 0x000B);     // y = 0 baseline, x = 0 lsb, integer scaling
        put16(head, face->units_per_EM);
        for (int i = 0; i < 4; ++i) put32(head, 0); // created, modified
        for (const int v: font_box) put16(head, v);
        put16(head, (bold ? 1 : 0) | (italic ? 2 : 0));
        put16(head, 8);  // lowestRecPPEM
        put16(head, 2);  // fontDirectionHint
        put16(head, 1);  // indexToLocFormat: 32-bit offsets
        put16(head, 0);  // glyphDataFormat

        std::string hhea;
        put32(hhea, 0x00010000);
        put16(hhea, face->ascender);
        put16(hhea, face->descender);
        put16(hhea, std::max(0, face->height - face->ascender + face->descender)); // lineGap
        put16(hhea, advance_max);
        put16(hhea, lsb_min);
        put16(hhea, rsb_min);
        put16(hhea, extent_max);
        put16(hhea, 1); // caret slope rise
        put16(hhea, 0); // run
        put16(hhea, 0); // offset
        for (int i = 0; i < 4; ++i) put16(hhea, 0);
        put16(hhea, 0); // metricDataFormat
        put16(hhea, glyph_count);

        std::string maxp;
        put32(maxp, 0x00010000);
        put16(maxp, glyph_count);
        put16(maxp, max_points);
        put16(maxp, max_contours);
        put16(maxp, 0); // composite points
        put16(maxp, 0); // composite contours
        put16(maxp, 2); // zones
        for (int i = 0; i < 7; ++i) put16(maxp, 0); // twilight points, storage, functions, instruction defs, stack, instructions, components
        put16(maxp, 0); // component depth

        std::string post;
        put32(post, 0x00030000); // version 3: no glyph names
        put32(post, post_table ? static_cast<std::uint32_t>(post_table->italicAngle) : 0);
        put16(post, post_table ? post_table->underlinePosition : 0);
        put16(post, post_table ? post_table->underlineThickness : 0);
        put32(post, post_table ? post_table->isFixedPitch : 0);
        for (int i = 0; i < 4; ++i) put32(post, 0);

        const std::string program = font_program({{"glyf", glyf}, {"head", head}, {"hhea", hhea}, {"hmtx", hmtx}, {"loca", loca}, {"maxp", maxp}, {"post", post}});

        // The subset's tag (9.6.4): six capital letters, from the glyphs it holds.
        std::uint32_t hash = 2166136261u;
        for (const auto &[glyph, character]: used) hash = (hash ^ glyph) * 16777619u;
        std::string tag;
        for (int i = 0; i < 6; ++i, hash /= 26) tag += static_cast<char>('A' + hash % 26);
        const pdflite::name base{tag + "+" + data_->name};

        // ToUnicode (9.10.3): each glyph to the character it was used for.
        std::string cmap = "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
                           "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
                           "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
                           "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";
        std::vector<std::string> entries;
        for (std::size_t g = 1; g < used.size(); ++g) {
            const char32_t c = used[g].second;
            std::string utf16;
            if (c < 0x10000) put16(utf16, static_cast<int>(c));
            else {
                put16(utf16, static_cast<int>(0xD800 + ((c - 0x10000) >> 10)));
                put16(utf16, static_cast<int>(0xDC00 + ((c - 0x10000) & 0x3FF)));
            }
            std::string code;
            put16(code, static_cast<int>(g));
            entries.push_back(string{code, true}.write() + " " + string{utf16, true}.write());
        }
        for (std::size_t at = 0; at < entries.size(); at += 100) {
            const auto count = std::min<std::size_t>(100, entries.size() - at);
            cmap += std::to_string(count) + " beginbfchar\n";
            for (std::size_t k = 0; k < count; ++k) cmap += entries[at + k] + "\n";
            cmap += "endbfchar\n";
        }
        cmap += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";

        const auto *os2 = static_cast<const TT_OS2 *>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2));
        const double cap = os2 && os2->version >= 2 && os2->sCapHeight > 0 ? os2->sCapHeight : face->ascender * 0.7;
        const double weight = os2 ? os2->usWeightClass : 400;

        const std::uint32_t descendant = out.allocate(), descriptor = out.allocate(), program_id = out.allocate(), unicode = out.allocate();
        out.write_stream(program_id, dictionary{{"Length1", program.size()}}, program);
        out.write_stream(unicode, {}, cmap);
        int flags = 4; // symbolic: a CID font's glyphs aren't the standard Latin set
        if (FT_IS_FIXED_WIDTH(face)) flags |= 1;
        if (italic) flags |= 64;
        out.write(descriptor, dictionary{
                {"Type", pdflite::name{"FontDescriptor"}}, {"FontName", base}, {"Flags", flags},
                {"FontBBox", array{std::round(units(face, font_box[0])), std::round(units(face, font_box[1])), std::round(units(face, font_box[2])), std::round(units(face, font_box[3]))}},
                {"ItalicAngle", post_table ? post_table->italicAngle / 65536.0 : 0.0},
                {"Ascent", std::round(units(face, face->ascender))}, {"Descent", std::round(units(face, face->descender))},
                {"CapHeight", std::round(units(face, cap))},
                {"StemV", std::round(10 + 220 * (weight - 50) / 900)}, // an estimate from the weight, as readers expect one
                {"FontFile2", reference{program_id}}});
        out.write(descendant, dictionary{
                {"Type", pdflite::name{"Font"}}, {"Subtype", pdflite::name{"CIDFontType2"}}, {"BaseFont", base},
                {"CIDSystemInfo", dictionary{{"Registry", string{"Adobe"}}, {"Ordering", string{"Identity"}}, {"Supplement", 0}}},
                {"FontDescriptor", reference{descriptor}}, {"W", array{0, widths}}, {"CIDToGIDMap", pdflite::name{"Identity"}}});
        out.write(id, dictionary{
                {"Type", pdflite::name{"Font"}}, {"Subtype", pdflite::name{"Type0"}}, {"BaseFont", base}, {"Encoding", pdflite::name{"Identity-H"}},
                {"DescendantFonts", array{reference{descendant}}}, {"ToUnicode", reference{unicode}}});
    }
}
