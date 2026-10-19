#ifndef SC_PDFLITE_CONTENT_H
#define SC_PDFLITE_CONTENT_H

#include <string>
#include <string_view>
#include <vector>

// sc-pdflite's content streams (ISO 32000-1, 8 and 9): a page's drawing, operator by operator.
namespace sc::pdflite {

    // A page's content stream, built operator by operator (ISO 32000-1, 8 and 9), in PDF's own
    // coordinates: points, y up from the bottom of the page. It keeps PDF's rules, throwing
    // std::logic_error when an operator would break them: graphics states (q/Q) balance, text only
    // inside BT/ET, a path painted or ended before anything else, Tf before text is shown, and no
    // q/Q, path or nested BT inside a text object. text() holds what's been written so far.
    class content {
    public:
        // Graphics state.
        void save();                  // q
        void restore();               // Q
        void transform(double a, double b, double c, double d, double e, double f); // cm
        void line_width(double width);                                             // w
        void fill_colour(double red, double green, double blue);                   // rg, 0 to 1
        void stroke_colour(double red, double green, double blue);                 // RG, 0 to 1
        void state(std::string_view resource);                                     // gs, a resource name such as "GS1"

        // Paths.
        void move(double x, double y);                                             // m
        void line(double x, double y);                                             // l
        void curve(double x1, double y1, double x2, double y2, double x3, double y3); // c
        void close_path();                                                         // h
        void rectangle(double x, double y, double width, double height);           // re

        // Painting a path (and ending it), or making it the clip.
        void fill(bool even_odd = false);              // f, f*
        void stroke();                                 // S
        void fill_and_stroke(bool even_odd = false);   // B, B*
        void end_path();                               // n
        void clip(bool even_odd = false);              // W, W*; the path is still to be painted or ended

        // Text.
        void begin_text(); // BT
        void end_text();   // ET
        void font(std::string_view resource, double size); // Tf, a resource name such as "F1"
        void text_position(double x, double y);            // Td
        // Tj: bytes in the font's encoding (for the built-in fonts, WinAnsiEncoding).
        void show(std::string_view bytes);

        [[nodiscard]] const std::string &text() const { return text_; }

        // Whether it ends where it may: no unbalanced q, open text object or unfinished path.
        [[nodiscard]] bool complete() const { return saved_.empty() && !in_text_ && !in_path_; }

    private:
        void op(std::string_view line);
        void outside_text(std::string_view what) const;
        void path_step(std::string_view what);
        void paint(std::string_view line);

        std::string text_;
        std::vector<bool> saved_;  // each open q: whether a font was set when it saved the state
        bool in_text_ = false;
        bool font_set_ = false;    // Tf given in the current graphics state
        bool in_path_ = false;
        bool clipping_ = false;  // W given, waiting for the painting operator
    };
}

#endif //SC_PDFLITE_CONTENT_H
