#include "pdflite/merge.h"

#include "pdflite/document.h"
#include "pdflite/file.h"
#include "pdflite/jpeg.h"
#include "pdflite/png.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <functional>
#include <random>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <system_error>
#include <unordered_map>

namespace sc::pdflite {
    namespace {
        bool has_type(const object &value, const std::string_view type) {
            if (!value.is<dictionary>()) return false;
            const auto *found = value.as<dictionary>().find("Type");
            return found && found->is<name>() && found->as<name>().text() == type;
        }

        // One input's objects copied into the new file: each written once, under a new number, the
        // references between them renumbered.
        class copier {
        public:
            copier(const reader &in, file &out) : in_(in), out_(out) {
                // The input's own /Encrypt dictionary has no place in the new file.
                if (const auto *encrypt = in.trailer().find("Encrypt"); encrypt && encrypt->is<reference>()) dropped_.insert(encrypt->as<reference>().id);
            }

            // Each page under a new number, known before anything refers to it: links and
            // annotations name pages.
            std::vector<std::uint32_t> number_pages() {
                std::vector<std::uint32_t> out;
                for (const auto id: in_.pages()) {
                    const auto mine = out_.allocate();
                    pages_.try_emplace(id, mine); // a page that's in the tree twice: references go to the first
                    out.push_back(mine);
                }
                return out;
            }

            // Page index, written as number under parent; adjust (when given) changes the copy first.
            void write_page(const std::size_t index, const std::uint32_t number, const std::uint32_t parent,
                            const std::function<void(dictionary &copy, const dictionary &theirs)> &adjust = {}) {
                const dictionary theirs = in_.page(index);
                dictionary page;
                page.add("Type", name{"Page"});
                page.add("Parent", reference{parent});
                for (const auto &[key, value]: theirs) {
                    const auto &k = key.text();
                    // Its old parent, and what ties it to structures that aren't copied (article
                    // beads, the tagged structure tree).
                    if (k == "Type" || k == "Parent" || k == "B" || k == "StructParents") continue;
                    if (object copied = copy(value); !copied.is<std::nullptr_t>()) page.add(k, std::move(copied));
                }
                if (adjust) adjust(page, theirs);
                out_.write(number, page);
            }

            // value with its references renumbered, the objects they name queued to be written.
            object copy(const object &value) {
                if (value.is<reference>()) return copy_reference(value.as<reference>().id);
                if (value.is<array>()) {
                    array out;
                    for (const auto &item: value.as<array>()) out.push_back(copy(item));
                    return out;
                }
                if (value.is<dictionary>()) {
                    dictionary out;
                    for (const auto &[key, item]: value.as<dictionary>())
                        if (object copied = copy(item); !copied.is<std::nullptr_t>()) out.add(key.text(), std::move(copied));
                    return out;
                }
                return value;
            }

            // Writes every object queued (and those they refer to in turn).
            void flush() {
                while (!queue_.empty()) {
                    const auto [theirs, mine] = queue_.back();
                    queue_.pop_back();
                    if (auto data = in_.stream(theirs)) {
                        dictionary entries;
                        for (const auto &[key, item]: data->entries) {
                            if (key.text() == "Length") continue; // written for it
                            if (object copied = copy(item); !copied.is<std::nullptr_t>()) entries.add(key.text(), std::move(copied));
                        }
                        out_.write_stream(mine, std::move(entries), data->data, false); // still encoded as it was
                    } else {
                        out_.write(mine, copy(in_.get(theirs)));
                    }
                }
            }

            [[nodiscard]] const reader &in() const { return in_; }

        private:
            object copy_reference(const std::uint32_t theirs) {
                if (const auto page = pages_.find(theirs); page != pages_.end()) return reference{page->second};
                if (const auto done = copied_.find(theirs); done != copied_.end()) return reference{done->second};
                if (dropped_.contains(theirs)) return nullptr;
                const object target = in_.get(theirs);
                // A reference to nothing is a reference to null; the old page tree and catalog aren't
                // copied (pages are, by number_pages()), nor is a page that isn't (a decoration's).
                if ((target.is<std::nullptr_t>() && !in_.is_stream(theirs)) || has_type(target, "Pages") || has_type(target, "Catalog") ||
                    has_type(target, "Page")) {
                    dropped_.insert(theirs);
                    return nullptr;
                }
                const auto mine = out_.allocate();
                copied_.emplace(theirs, mine);
                queue_.emplace_back(theirs, mine);
                return reference{mine};
            }

            const reader &in_;
            file &out_;
            std::unordered_map<std::uint32_t, std::uint32_t> pages_;
            std::unordered_map<std::uint32_t, std::uint32_t> copied_;
            std::set<std::uint32_t> dropped_;
            std::vector<std::pair<std::uint32_t, std::uint32_t>> queue_;
        };

        // The new file: written beside output and renamed when done (output may be one of the
        // inputs, and a failure leaves it as it was), encrypted when asked, with one page tree.
        // write() fills it, given the file, the tree's number, the kids to add to and the catalog
        // and information to complete.
        void write_file(const std::filesystem::path &output, const std::optional<protection> &protect,
                        const std::function<void(file &out, std::uint32_t tree, array &kids, dictionary &catalog, std::uint32_t &info)> &write) {
            std::filesystem::path part = output;
            part += ".part";
            try {
                file out{part};
                std::uint32_t encrypt = 0;
                if (protect) {
                    const auto security = std::make_shared<const encryption>(*protect);
                    encrypt = out.allocate();
                    out.encrypt(security, encrypt); // everything after it is encrypted, but its own dictionary
                    out.write(encrypt, security->dictionary());
                }
                const auto catalog_id = out.allocate();
                const auto tree = out.allocate();
                array kids;
                dictionary catalog{{"Type", name{"Catalog"}}, {"Pages", reference{tree}}};
                std::uint32_t info = 0;
                write(out, tree, kids, catalog, info);
                if (kids.empty()) throw std::invalid_argument("sc::pdflite: no pages to write (readers refuse a PDF without any)");
                const auto count = kids.size();
                out.write(tree, dictionary{{"Type", name{"Pages"}}, {"Kids", std::move(kids)}, {"Count", count}});
                out.write(catalog_id, catalog);
                out.finish(catalog_id, info, encrypt);
            } catch (...) {
                std::error_code ignored;
                std::filesystem::remove(part, ignored);
                throw;
            }
            std::error_code error;
            std::filesystem::rename(part, output, error);
            if (error) {
                std::filesystem::remove(part, error);
                throw std::runtime_error("sc::pdflite: can't write " + output.string());
            }
        }

        // An input's document information, bookmarks and named destinations, copied into the new
        // catalog and information.
        void copy_document(copier &copy, file &out, dictionary &catalog, std::uint32_t &info) {
            const reader &in = copy.in();
            if (const auto *found = in.trailer().find("Info")) {
                if (const object copied = copy.copy(*found); copied.is<reference>()) {
                    info = copied.as<reference>().id;
                } else if (copied.is<dictionary>()) {
                    info = out.allocate();
                    out.write(info, copied);
                }
            }
            const dictionary theirs = in.catalog();
            if (const auto *outlines = theirs.find("Outlines")) {
                if (object copied = copy.copy(*outlines); !copied.is<std::nullptr_t>()) {
                    catalog.add("Outlines", std::move(copied));
                    if (const auto *mode = theirs.find("PageMode"); mode && mode->is<name>()) catalog.add("PageMode", *mode);
                }
            }
            if (const auto *dests = theirs.find("Dests"))
                if (object copied = copy.copy(*dests); !copied.is<std::nullptr_t>()) catalog.add("Dests", std::move(copied));
            if (const auto *names = theirs.find("Names")) {
                if (const object resolved = in.resolve(*names); resolved.is<dictionary>()) {
                    if (const auto *dests = resolved.as<dictionary>().find("Dests")) {
                        if (object copied = copy.copy(*dests); !copied.is<std::nullptr_t>())
                            catalog.add("Names", dictionary{{"Dests", std::move(copied)}});
                    }
                }
            }
            if (const auto *layout = theirs.find("PageLayout"); layout && layout->is<name>()) catalog.add("PageLayout", *layout);
        }

        // ---- Decorations ----

        // An affine transform as PDF's cm has it: x' = a x + c y + e, y' = b x + d y + f.
        struct matrix {
            double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;

            // This transform, then then.
            [[nodiscard]] matrix then(const matrix &next) const {
                return {a * next.a + b * next.c, a * next.b + b * next.d, c * next.a + d * next.c, c * next.b + d * next.d,
                        e * next.a + f * next.c + next.e, e * next.b + f * next.d + next.f};
            }

            [[nodiscard]] matrix inverse() const {
                const double det = a * d - b * c;
                if (det == 0) throw std::invalid_argument("sc::pdflite: a transform that can't be undone");
                return {d / det, -b / det, -c / det, a / det, (c * f - d * e) / det, (b * e - a * f) / det};
            }

            [[nodiscard]] std::string cm() const {
                // Close to whole numbers they're written whole: 0.99999999 would be 1.0000 anyway.
                auto n = [](const double v) { return number(std::abs(v - std::round(v)) < 1e-9 ? std::round(v) : v); };
                return n(a) + " " + n(b) + " " + n(c) + " " + n(d) + " " + n(e) + " " + n(f) + " cm";
            }
        };

        double number_in(const reader &in, const object &value) {
            const object resolved = in.resolve(value);
            if (resolved.is<std::int64_t>()) return static_cast<double>(resolved.as<std::int64_t>());
            if (resolved.is<double>()) return resolved.as<double>();
            throw std::runtime_error("sc::pdflite: a page box with something other than numbers in it");
        }

        // A page's visible area (its crop box, else its media box; 14.11.2) and turn, as shown.
        struct page_frame {
            double x0 = 0, y0 = 0, width = 612, height = 792;
            int rotate = 0; // 0, 90, 180 or 270, clockwise

            // The page as shown: its size, and from its user space to there (origin bottom-left).
            [[nodiscard]] double shown_width() const { return rotate % 180 == 0 ? width : height; }
            [[nodiscard]] double shown_height() const { return rotate % 180 == 0 ? height : width; }

            [[nodiscard]] matrix to_shown() const {
                switch (rotate) {
                    case 90: return {0, -1, 1, 0, -y0, x0 + width};
                    case 180: return {-1, 0, 0, -1, x0 + width, y0 + height};
                    case 270: return {0, 1, -1, 0, y0 + height, -x0};
                    default: return {1, 0, 0, 1, -x0, -y0};
                }
            }
        };

        page_frame frame_of(const reader &in, const dictionary &page) {
            page_frame out;
            const auto *box = page.find("CropBox");
            if (!box) box = page.find("MediaBox");
            if (box) {
                const object resolved = in.resolve(*box);
                if (resolved.is<array>() && resolved.as<array>().size() == 4) {
                    const auto &corners = resolved.as<array>();
                    const double x1 = number_in(in, corners[0]), y1 = number_in(in, corners[1]);
                    const double x2 = number_in(in, corners[2]), y2 = number_in(in, corners[3]);
                    out.x0 = std::min(x1, x2);
                    out.y0 = std::min(y1, y2);
                    out.width = std::abs(x2 - x1);
                    out.height = std::abs(y2 - y1);
                }
            }
            if (const auto *rotate = page.find("Rotate")) {
                const object resolved = in.resolve(*rotate);
                if (resolved.is<std::int64_t>()) out.rotate = static_cast<int>(((resolved.as<std::int64_t>() % 360) + 360) % 360) / 90 * 90;
            }
            if (out.width <= 0 || out.height <= 0) throw std::runtime_error("sc::pdflite: a page of no size");
            return out;
        }

        // From the decoration's page's user space into the page's: turned upright as each is shown,
        // scaled to fit (keeping its proportions) and centred.
        matrix placement(const page_frame &decoration, const page_frame &page) {
            const double scale = std::min(page.shown_width() / decoration.shown_width(), page.shown_height() / decoration.shown_height());
            const matrix fit{scale, 0, 0, scale, (page.shown_width() - scale * decoration.shown_width()) / 2,
                             (page.shown_height() - scale * decoration.shown_height()) / 2};
            return decoration.to_shown().then(fit).then(page.to_shown().inverse());
        }

        // A decoration's pages as form XObjects (8.10), each written once, the first time it's used.
        class decorations {
        public:
            decorations(copier &copy, file &out) : copy_(copy), out_(out) {
                if (copy.in().page_count() == 0) throw std::invalid_argument("sc::pdflite: a decoration without pages");
            }

            // For page index of the input: the form (its number) and its frame.
            std::pair<std::uint32_t, page_frame> for_page(const std::size_t index) {
                const auto which = std::min(index, copy_.in().page_count() - 1);
                if (const auto found = forms_.find(which); found != forms_.end()) return found->second;
                return forms_.emplace(which, make(which)).first->second;
            }

        private:
            std::pair<std::uint32_t, page_frame> make(const std::size_t which) {
                const reader &in = copy_.in();
                const dictionary page = in.page(which);
                const page_frame frame = frame_of(in, page);
                dictionary form{{"Type", name{"XObject"}}, {"Subtype", name{"Form"}},
                                {"BBox", array{frame.x0, frame.y0, frame.x0 + frame.width, frame.y0 + frame.height}}};
                if (const auto *resources = page.find("Resources"))
                    if (object copied = copy_.copy(*resources); !copied.is<std::nullptr_t>()) form.add("Resources", std::move(copied));
                if (const auto *group = page.find("Group")) // a transparency group stays one
                    if (object copied = copy_.copy(*group); !copied.is<std::nullptr_t>()) form.add("Group", std::move(copied));
                // Its content: one stream copied as it's encoded, or several joined (decoded first).
                std::vector<std::uint32_t> parts;
                if (const auto *contents = page.find("Contents")) {
                    if (contents->is<reference>() && in.is_stream(contents->as<reference>().id)) {
                        parts.push_back(contents->as<reference>().id);
                    } else if (const object list = in.resolve(*contents); list.is<array>()) {
                        for (const auto &part: list.as<array>())
                            if (part.is<reference>() && in.is_stream(part.as<reference>().id)) parts.push_back(part.as<reference>().id);
                    }
                }
                const auto id = out_.allocate();
                if (parts.size() == 1) {
                    const auto data = *in.stream(parts[0]);
                    for (const auto key: {"Filter", "DecodeParms"})
                        if (const auto *value = data.entries.find(key))
                            if (object copied = copy_.copy(*value); !copied.is<std::nullptr_t>()) form.add(key, std::move(copied));
                    out_.write_stream(id, std::move(form), data.data, false);
                } else {
                    std::string joined;
                    for (const auto part: parts) joined += in.decoded(*in.stream(part)) + "\n";
                    out_.write_stream(id, std::move(form), joined);
                }
                return {id, frame};
            }

            copier &copy_;
            file &out_;
            std::map<std::size_t, std::pair<std::uint32_t, page_frame>> forms_;
        };

        // Content streams of the new file's own, compressed: the drawing around a page's content,
        // each written once and shared by the pages that draw the same.
        class small_streams {
        public:
            explicit small_streams(file &out) : out_(out) {}

            reference operator()(const std::string &content) {
                if (const auto found = written_.find(content); found != written_.end()) return found->second;
                const auto id = out_.allocate();
                out_.write_stream(id, dictionary{}, content);
                return written_.emplace(content, reference{id}).first->second;
            }

        private:
            file &out_;
            std::map<std::string, reference> written_;
        };

        // The page's resources with the form added under a name of its own: a copy, so resources
        // other pages share aren't changed.
        std::string add_form(copier &copy, dictionary &page, const dictionary &theirs, const std::uint32_t form) {
            const reader &in = copy.in();
            const object resources = theirs.find("Resources") ? in.resolve(*theirs.find("Resources")) : object{dictionary{}};
            dictionary mine = resources.is<dictionary>() ? copy.copy(resources).as<dictionary>() : dictionary{};
            dictionary forms;
            if (resources.is<dictionary>()) {
                if (const auto *xobjects = resources.as<dictionary>().find("XObject"))
                    if (const object listed = in.resolve(*xobjects); listed.is<dictionary>()) forms = copy.copy(listed).as<dictionary>();
            }
            std::string label = "SCDecoration";
            for (int n = 2; forms.contains(label); ++n) label = "SCDecoration" + std::to_string(n);
            forms.add(label, reference{form});
            mine.set("XObject", std::move(forms));
            page.set("Resources", std::move(mine));
            return label;
        }

        // The page's content streams, copied, in order: one stream, or an array of them (itself
        // perhaps a reference); none for a page without content.
        std::vector<object> contents_of(copier &copy, const dictionary &original) {
            std::vector<object> out;
            const auto *contents = original.find("Contents");
            if (!contents) return out;
            const reader &in = copy.in();
            if (contents->is<reference>() && in.is_stream(contents->as<reference>().id)) {
                out.push_back(copy.copy(*contents));
            } else if (const object list = in.resolve(*contents); list.is<array>()) {
                for (const auto &part: list.as<array>())
                    if (object copied = copy.copy(part); copied.is<reference>()) out.push_back(std::move(copied));
            }
            return out;
        }
    }

    namespace {
        // ---- Pictures as pages ----

        enum class picture { none, jpeg, png };

        // A JPEG or PNG, by its first bytes (not its name).
        picture picture_in(const std::filesystem::path &file) {
            std::ifstream in{file, std::ios::binary};
            char head[8] = {};
            in.read(head, sizeof head);
            const std::string_view bytes{head, static_cast<std::size_t>(in.gcount())};
            if (bytes.starts_with("\xFF\xD8\xFF")) return picture::jpeg;
            if (bytes.starts_with("\x89PNG\r\n\x1A\n")) return picture::png;
            return picture::none;
        }

        // A JPEG's EXIF orientation (1 to 8; 1, upright, when it has none): how the camera stored it.
        int exif_orientation(const std::string &jpeg) {
            auto u16 = [&](const std::size_t at, const bool little) {
                const auto b0 = static_cast<unsigned char>(jpeg[at]), b1 = static_cast<unsigned char>(jpeg[at + 1]);
                return little ? b0 | b1 << 8 : b0 << 8 | b1;
            };
            auto u32 = [&](const std::size_t at, const bool little) {
                return little ? static_cast<std::uint32_t>(u16(at, true)) | static_cast<std::uint32_t>(u16(at + 2, true)) << 16
                              : static_cast<std::uint32_t>(u16(at, false)) << 16 | static_cast<std::uint32_t>(u16(at + 2, false));
            };
            // The segments before the image data: APP1 "Exif\0\0" holds a TIFF header and IFD0.
            for (std::size_t at = 2; at + 4 <= jpeg.size();) {
                if (static_cast<unsigned char>(jpeg[at]) != 0xFF) break;
                const auto marker = static_cast<unsigned char>(jpeg[at + 1]);
                if (marker == 0xDA || marker == 0xD9) break; // the image data: no more segments
                const std::size_t length = u16(at + 2, false);
                if (length < 2 || at + 2 + length > jpeg.size()) break;
                if (marker == 0xE1 && length >= 8 && jpeg.compare(at + 4, 6, std::string{"Exif\0\0", 6}) == 0) {
                    const std::size_t tiff = at + 10, end = at + 2 + length;
                    if (tiff + 8 > end) break;
                    const bool little = jpeg.compare(tiff, 2, "II") == 0;
                    if (!little && jpeg.compare(tiff, 2, "MM") != 0) break;
                    const std::size_t ifd = tiff + u32(tiff + 4, little);
                    if (ifd + 2 > end) break;
                    const auto entries = u16(ifd, little);
                    for (std::size_t i = 0; i < entries; ++i) {
                        const std::size_t entry = ifd + 2 + i * 12;
                        if (entry + 12 > end) break;
                        if (u16(entry, little) == 0x0112) {
                            const int value = u16(entry + 8, little);
                            return value >= 1 && value <= 8 ? value : 1;
                        }
                    }
                    break;
                }
                at += 2 + length;
            }
            return 1;
        }

        // From the picture's unit square (as it's stored) to it turned upright as its EXIF
        // orientation asks, w by h pixels stored; y down. Orientations 5 to 8 turn it a quarter.
        sc::transform upright(const int orientation, const double w, const double h) {
            switch (orientation) {
                case 2: return {-w, 0, 0, h, w, 0};  // mirrored left to right
                case 3: return {-w, 0, 0, -h, w, h}; // turned half round
                case 4: return {w, 0, 0, -h, 0, h};  // mirrored top to bottom
                case 5: return {0, w, h, 0, 0, 0};   // mirrored across the diagonal
                case 6: return {0, w, -h, 0, h, 0};  // turned a quarter clockwise to be upright
                case 7: return {0, -w, -h, 0, h, w}; // mirrored across the other diagonal
                case 8: return {0, -w, h, 0, 0, w};  // turned a quarter anticlockwise
                default: return {w, 0, 0, h, 0, 0};
            }
        }

        // A one-page PDF, written to a temporary file, of the picture on a page of size (points):
        // turned upright, scaled to fit keeping its proportions, and centred.
        std::filesystem::path picture_page(const std::filesystem::path &file, const picture kind, const double width, const double height) {
            std::random_device random;
            const auto out = std::filesystem::temp_directory_path() / ("sc-pdflite-picture-" + std::to_string(random()) + std::to_string(random()) + ".pdf");
            std::unique_ptr<image> drawn;
            int orientation = 1;
            if (kind == picture::jpeg) {
                std::ifstream in{file, std::ios::binary};
                std::string bytes{std::istreambuf_iterator<char>{in}, {}};
                orientation = exif_orientation(bytes);
                drawn = std::make_unique<jpeg>(std::move(bytes));
            } else {
                drawn = std::make_unique<png>(png::load(file));
            }
            const auto pixels = drawn->size();
            const double w = pixels.width(), h = pixels.height();
            if (w <= 0 || h <= 0) throw std::runtime_error("sc::pdflite: " + file.string() + " is a picture of no size");
            const bool quarter = orientation >= 5;
            const double shown_w = quarter ? h : w, shown_h = quarter ? w : h;
            const double scale = std::min(width / shown_w, height / shown_h);
            const auto placement = upright(orientation, w, h)
                                       .then(sc::transform::scale(scale))
                                       .then(sc::transform::translate((width - scale * shown_w) / 2, (height - scale * shown_h) / 2));
            document pdf{out, sc::size_<sc::distance>{width, height}};
            const auto name = pdf.image(*drawn);
            pdf.page_content().draw(name, placement);
            pdf.close();
            return out;
        }

        // The size a page is shown at (its crop box, turned as /Rotate turns it), in points.
        std::pair<double, double> shown_size(const reader &in, const std::size_t page) {
            const auto frame = frame_of(in, in.page(page));
            return {frame.shown_width(), frame.shown_height()};
        }

        // Temporary files, removed when it goes, however it goes.
        struct temporaries {
            std::vector<std::filesystem::path> files;
            ~temporaries() {
                std::error_code ignored;
                for (const auto &file: files) std::filesystem::remove(file, ignored);
            }
        };
    }

    void merge(const std::vector<std::reference_wrapper<const reader>> &inputs, const std::filesystem::path &output,
               const std::optional<protection> &protect) {
        write_file(output, protect, [&](file &out, const std::uint32_t tree, array &kids, dictionary &catalog, std::uint32_t &info) {
            for (std::size_t n = 0; n < inputs.size(); ++n) {
                copier copy{inputs[n].get(), out};
                const auto numbers = copy.number_pages();
                for (std::size_t i = 0; i < numbers.size(); ++i) {
                    copy.write_page(i, numbers[i], tree);
                    kids.push_back(reference{numbers[i]});
                }
                if (n == 0) copy_document(copy, out, catalog, info);
                copy.flush();
            }
        });
    }

    void merge(const std::vector<std::filesystem::path> &inputs, const std::filesystem::path &output, const std::optional<protection> &protect) {
        // PDFs read first; each picture then becomes a page the size of the page before it (the
        // first page after it, when it comes first; A4 when there's no PDF at all).
        std::vector<std::optional<reader>> read(inputs.size());
        std::vector<picture> kinds(inputs.size(), picture::none);
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            kinds[i] = picture_in(inputs[i]);
            if (kinds[i] != picture::none) continue;
            try {
                read[i].emplace(inputs[i]);
            } catch (const std::runtime_error &error) {
                throw std::runtime_error(inputs[i].string() + ": " + error.what());
            }
        }
        temporaries made;
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            if (kinds[i] == picture::none) continue;
            std::pair<double, double> size{595.2756, 841.8898}; // A4
            bool found = false;
            for (std::size_t j = i; j-- > 0 && !found;)
                if (read[j] && read[j]->page_count() > 0) size = shown_size(*read[j], read[j]->page_count() - 1), found = true;
            for (std::size_t j = i + 1; j < inputs.size() && !found; ++j)
                if (read[j] && read[j]->page_count() > 0) size = shown_size(*read[j], 0), found = true;
            try {
                made.files.push_back(picture_page(inputs[i], kinds[i], size.first, size.second));
                read[i].emplace(made.files.back());
            } catch (const std::exception &error) {
                throw std::runtime_error(inputs[i].string() + ": " + error.what());
            }
        }
        std::vector<std::reference_wrapper<const reader>> refs;
        for (const auto &each: read) refs.emplace_back(*each);
        merge(refs, output, protect);
    }

    void merge(const std::initializer_list<std::filesystem::path> inputs, const std::filesystem::path &output,
               const std::optional<protection> &protect) {
        merge(std::vector<std::filesystem::path>{inputs}, output, protect);
    }

    void protect(const std::filesystem::path &input, const std::filesystem::path &output, const protection &settings, const std::string_view password) {
        const reader in{input, password};
        merge({std::cref(in)}, output, settings);
    }

    void decorate(const reader &input, const reader &decoration, const std::filesystem::path &output, const layer where,
                  const std::optional<protection> &protect) {
        write_file(output, protect, [&](file &out, const std::uint32_t tree, array &kids, dictionary &catalog, std::uint32_t &info) {
            copier copy{input, out};
            copier theirs{decoration, out};
            decorations forms{theirs, out};
            small_streams small_stream{out};
            const auto numbers = copy.number_pages();
            for (std::size_t i = 0; i < numbers.size(); ++i) {
                copy.write_page(i, numbers[i], tree, [&](dictionary &page, const dictionary &original) {
                    const auto [form, frame] = forms.for_page(i);
                    const std::string label = add_form(copy, page, original, form);
                    const std::string draw = "q " + placement(frame, frame_of(input, original)).cm() + " /" + label + " Do Q\n";
                    array contents;
                    if (where == layer::behind) {
                        contents.push_back(small_stream(draw));
                        for (auto &part: contents_of(copy, original)) contents.push_back(std::move(part));
                    } else {
                        // The page's own drawing kept to itself (it may leave the graphics state
                        // changed), the stamp over it.
                        contents.push_back(small_stream("q\n"));
                        for (auto &part: contents_of(copy, original)) contents.push_back(std::move(part));
                        contents.push_back(small_stream("Q\n" + draw));
                    }
                    page.set("Contents", std::move(contents));
                });
                kids.push_back(reference{numbers[i]});
            }
            copy_document(copy, out, catalog, info);
            copy.flush();
            theirs.flush();
        });
    }

    void decorate(const std::filesystem::path &input, const std::filesystem::path &decoration, const std::filesystem::path &output,
                  const layer where, const std::optional<protection> &protect) {
        const reader pages{input};
        const reader backdrop{decoration};
        decorate(pages, backdrop, output, where, protect);
    }
}
