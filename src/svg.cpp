#include "pdflite/svg.h"

#include "pdflite/content.h"
#include "pdflite/jpeg.h"
#include "pdflite/object.h"
#include "pdflite/png.h"

#include <standard_font.h>

#include <map>
#include <stdexcept>
#include <utility>

namespace sc::pdflite {
    namespace {
        // Text in WinAnsiEncoding, a character it doesn't have as '?'.
        std::string win_ansi_text(const std::string &utf8) {
            std::string out;
            for (const char32_t code: code_points(utf8)) out += sc::standard_font::encode(code).value_or('?');
            return out;
        }

        // The built-in font for a label's font: its own (Helvetica, Times, Courier and their
        // look-alikes), else Helvetica of its weight and slant.
        sc::standard_font::face face_of(const sc::font &spec) {
            if (const auto standard = sc::standard_font::of(spec)) return standard->which();
            return sc::standard_font::of(sc::font{"Helvetica", spec.size, spec.weight, spec.italic})->which();
        }

        // What a form's content names, collected while it's written.
        struct form_resources {
            std::map<sc::standard_font::face, std::string> fonts;
            std::map<std::pair<double, double>, std::string> states; // fill and stroke alpha
            std::map<const std::string *, std::pair<std::shared_ptr<const image>, std::string>> images; // by a bitmap's bytes

            std::string font(const sc::standard_font::face which) {
                return fonts.try_emplace(which, "F" + std::to_string(fonts.size() + 1)).first->second;
            }

            // The graphics state for these alphas, or "" when both are opaque.
            std::string state(const double fill, const double stroke) {
                if (fill >= 1 && stroke >= 1) return {};
                return states.try_emplace({fill, stroke}, "GS" + std::to_string(states.size() + 1)).first->second;
            }

            std::string picture(const sc::bitmap &bitmap) {
                const std::string *key = &bitmap.bytes();
                if (const auto found = images.find(key); found != images.end()) return found->second.second;
                std::shared_ptr<const pdflite::image> picture;
                if (bitmap.type() == sc::bitmap::format::png) picture = std::make_shared<png>(bitmap.bytes());
                else picture = std::make_shared<jpeg>(bitmap.bytes());
                return images.emplace(key, std::pair{picture, "Im" + std::to_string(images.size() + 1)}).first->second.second;
            }
        };

        void clip_to(content &out, const std::optional<sc::path> &clip) {
            if (!clip) return;
            out.path(*clip);
            out.clip(); // nonzero, as sc::canvas clips
            out.end_path();
        }
    }

    svg svg::load(const std::filesystem::path &path) { return svg{sc::drawing::load_svg(path)}; }

    svg svg::parse(const std::string_view text) { return svg{sc::drawing::from_svg(text)}; }

    svg::svg(sc::drawing drawing) : drawing_(std::make_shared<const sc::drawing>(std::move(drawing))) {
        if (!(drawing_->page().width() > 0 && drawing_->page().height() > 0)) throw std::invalid_argument("sc::pdflite: an SVG drawing without a size");
    }

    sc::size svg::size() const { return drawing_->page(); }

    void svg::write(file &out, const std::uint32_t id) const {
        const double w = size().width(), h = size().height();
        // The drawing's own coordinates (y down) written y up in its box, [0 0 w h]; the form's
        // matrix makes that the unit square content::draw() places.
        content page{h};
        form_resources used;
        for (const auto &item: drawing_->items()) {
            if (const auto *shape = std::get_if<sc::drawable>(&item)) {
                const auto &look = shape->appearance;
                const bool stroked = look.stroke().visible(), filled = look.fill().visible();
                if (!stroked && !filled) continue;
                page.save();
                clip_to(page, shape->clip);
                if (const auto state = used.state(filled ? look.fill().colour().alpha() : 1, stroked ? look.stroke().colour().alpha() : 1); !state.empty())
                    page.state(state);
                if (filled) page.fill_colour(look.fill().colour().red(), look.fill().colour().green(), look.fill().colour().blue());
                if (stroked) {
                    page.stroke_colour(look.stroke().colour().red(), look.stroke().colour().green(), look.stroke().colour().blue());
                    page.line_width(look.stroke().width());
                }
                page.path(sc::to_path(shape->shape));
                page.paint(filled, stroked, look.fill().rule() == sc::fill_rule::even_odd);
                page.restore();
            } else if (const auto *text = std::get_if<sc::label>(&item)) {
                if (!text->fill.visible() || text->text.content.empty()) continue;
                const auto &colour = text->fill.colour();
                page.save();
                clip_to(page, text->clip);
                if (const auto state = used.state(colour.alpha(), 1); !state.empty()) page.state(state);
                page.fill_colour(colour.red(), colour.green(), colour.blue());
                page.begin_text();
                page.font(used.font(face_of(text->text.font)), text->text.font.size);
                page.text_position(text->text.position.x(), text->text.position.y());
                page.show(win_ansi_text(text->text.content));
                page.end_text();
                page.restore();
            } else if (const auto *figure = std::get_if<sc::figure>(&item)) {
                page.save();
                clip_to(page, figure->clip);
                page.draw(used.picture(figure->bitmap), figure->placement);
                page.restore();
            }
        }

        dictionary resources;
        if (!used.fonts.empty()) {
            dictionary fonts;
            for (const auto &[which, resource]: used.fonts) {
                const std::uint32_t font = out.allocate();
                out.write(font, dictionary{{"Type", name{"Font"}}, {"Subtype", name{"Type1"}}, {"BaseFont", name{sc::standard_font::name(which)}},
                                           {"Encoding", name{"WinAnsiEncoding"}}});
                fonts.add(resource, reference{font});
            }
            resources.add("Font", fonts);
        }
        if (!used.states.empty()) {
            // Transparency (11.6.4.4): ca for fills and text, CA for strokes.
            dictionary states;
            for (const auto &[alphas, resource]: used.states)
                states.add(resource, dictionary{{"Type", name{"ExtGState"}}, {"ca", alphas.first}, {"CA", alphas.second}});
            resources.add("ExtGState", states);
        }
        if (!used.images.empty()) {
            dictionary xobjects;
            for (const auto &[key, entry]: used.images) {
                const std::uint32_t picture = out.allocate();
                entry.first->write(out, picture);
                xobjects.add(entry.second, reference{picture});
            }
            resources.add("XObject", xobjects);
        }
        if (!page.complete()) throw std::logic_error("sc::pdflite: an SVG's content isn't complete");
        out.write_stream(id, dictionary{{"Type", name{"XObject"}}, {"Subtype", name{"Form"}}, {"BBox", array{0, 0, w, h}},
                                        {"Matrix", array{1 / w, 0, 0, 1 / h, 0, 0}}, {"Resources", resources}},
                         page.text());
    }
}
