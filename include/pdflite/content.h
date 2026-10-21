#ifndef SC_PDFLITE_CONTENT_H
#define SC_PDFLITE_CONTENT_H

#include <distance.h>
#include <path.h>
#include <rect.h>
#include <transform.h>

#include <string>
#include <string_view>
#include <vector>

// sc-pdflite's content streams (ISO 32000-1, 8 and 9): a page's drawing, operator by operator.
namespace sc::pdflite {
    // A page's content stream, built operator by operator (ISO 32000-1, 8 and 9). Positions are
    // sc-graphics' canvas's: from the page's top-left corner, y down, as sc::distance (sc::dist::mm,
    // sc::dist::inch, or a plain number of points); they're written in
    // PDF's own (y up from the bottom), so it needs the page's height. It keeps PDF's rules, throwing
    // std::logic_error when an operator would break them: graphics states (q/Q) balance, text only
    // inside BT/ET, a path painted or ended before anything else, Tf before text is shown, and no
    // q/Q, path or nested BT inside a text object. text() holds what's been written so far.
    class content {
    public:
        // For a page this many points high.
        explicit content(sc::distance page_height);

        // Graphics state.
        void save();                  // q
        void restore();               // Q
        // cm: x' = a x + c y + e, y' = b x + d y + f, in these coordinates (y down); written as the
        // same mapping in PDF's.
        void transform(double a, double b, double c, double d, sc::distance e, sc::distance f);
        void line_width(sc::distance width);                                       // w
        void fill_colour(double red, double green, double blue);                   // rg, 0 to 1
        void stroke_colour(double red, double green, double blue);                 // RG, 0 to 1
        void state(std::string_view resource);                                     // gs, a resource name such as "GS1"

        // Paths.
        void move(sc::distance x, sc::distance y);                                 // m
        void line(sc::distance x, sc::distance y);                                 // l
        void curve(sc::distance x1, sc::distance y1, sc::distance x2, sc::distance y2, sc::distance x3, sc::distance y3); // c
        void close_path();                                                         // h
        void rectangle(sc::distance left, sc::distance top, sc::distance width, sc::distance height); // re
        void path(const sc::path &shape);                                          // m, l, c and h, step by step

        // Painting a path (and ending it), or making it the clip.
        void fill(bool even_odd = false);              // f, f*
        void stroke();                                 // S
        void fill_and_stroke(bool even_odd = false);   // B, B*
        void end_path();                               // n
        void paint(bool filled, bool stroked, bool even_odd = false); // B, B*, f, f*, S, or n for neither
        void clip(bool even_odd = false);              // W, W*; the path is still to be painted or ended

        // Text.
        void begin_text(); // BT
        void end_text();   // ET
        void font(std::string_view resource, sc::distance size); // Tf, a resource name such as "F1"
        // Tm: the baseline starts at (x, y), wherever the text before it ended.
        void text_position(sc::distance x, sc::distance y);
        // Tj: bytes in the font's encoding (for the built-in fonts, WinAnsiEncoding).
        void show(std::string_view bytes);

        // A picture (Do, 8.8 and 8.10), a resource name such as "Im1" (document::image(): a JPEG,
        // PNG or SVG): its unit square, top-left corner at (0, 0) and bottom-right at (1, 1), drawn
        // through placement (in these coordinates, y down), so flips, turns and scales are
        // sc::transform's. Inside q ... Q, so the placement doesn't stay. Not in a text object, nor
        // while a path is open.
        void draw(std::string_view resource, const sc::transform &placement);

        // The same, filling where (stretched to it; keep the picture's proportions for an
        // undistorted one).
        void draw(std::string_view resource, const sc::rect &where);

        [[nodiscard]] const std::string &text() const { return text_; }

        // Whether it ends where it may: no unbalanced q, open text object or unfinished path.
        [[nodiscard]] bool complete() const { return saved_.empty() && !in_text_ && !in_path_; }

    private:
        void op(std::string_view line);
        void outside_text(std::string_view what) const;
        void path_step(std::string_view what);
        void finish_path(std::string_view line);

        [[nodiscard]] double y(double down) const { return height_ - down; } // PDF's y

        double height_;
        std::string text_;
        std::vector<bool> saved_;  // each open q: whether a font was set when it saved the state
        bool in_text_ = false;
        bool font_set_ = false;    // Tf given in the current graphics state
        bool in_path_ = false;
        bool clipping_ = false;  // W given, waiting for the painting operator
    };
}

#endif //SC_PDFLITE_CONTENT_H
