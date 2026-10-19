// Writes two password-protected PDFs with sc-pdflite (AES-256): one that asks for a password to
// open, and one anyone can open but only print, its other rights kept for the owner's password.
// Each line shows a call and what it returned.

#include <filesystem>
#include <iostream>
#include <string>

#include <params.h>
#include <sc.h>

#include <distance.h>
#include <pdflite.h>

// The units by their plain names: mm(10), inch(1), pdf.size().in<mm>().
using namespace sc::dist;

namespace {
    void page_of_text(sc::pdflite::document &pdf, const std::string &line) {
        const sc::pdflite::font font{sc::font{"sans-serif", 14}};
        auto &page = pdf.page_content();
        page.begin_text();
        page.font(pdf.font(font), 14);
        page.text_position(mm(20), mm(30));
        page.show(font.encode(line));
        page.end_text();
    }
}

int main(int argc, char **argv) {
    const sc::params args{argc, argv, {
        {"name", "sc-pdflite-passwords"},
        {"description", "Writes two password-protected PDFs with sc-pdflite (AES-256): one that asks for a password to "
                        "open (\"open sesame\"), and one anyone can open but only print (owner's password \"admin\")."},
        {"positional", {
            {"folder", {{"default", std::filesystem::temp_directory_path().string()}, {"help", "where to write them"}}}}}}};

    try {
        const std::filesystem::path folder = args.as<std::string>("folder");
        sc::console::title("simply-cpp pdflite: passwords and permissions");
        sc::timer sw;

        // [readme]
        sc::console::heading("A password to open it: everything in the file is encrypted");
        const auto locked = folder / "sc-pdflite-locked.pdf";
        sc::pdflite::document pdf{locked, sc::page_size::A5};
        page_of_text(pdf, "Opened with the password “open sesame”.");
        pdf.info("Title", "sc-pdflite: a password to open");
        pdf.info("Author", "Roelof Rossouw");
        pdf.info("Subject", "A PDF that asks for a password to open, encrypted with AES-256");
        pdf.info("Creator", "sc-pdflite-passwords");
        SC_STEP(pdf.protect({"open sesame", "admin"})); // user password, owner password
        SC_STEP(pdf.close());
        SC_SHOW(locked.string());

        sc::console::heading("Anyone may open it, and print; the rest takes the owner's password");
        const auto print_only = folder / "sc-pdflite-print-only.pdf";
        sc::pdflite::document open{print_only, sc::page_size::A5};
        page_of_text(open, "Anyone can open and print this; copying needs “admin”.");
        open.info("Title", "sc-pdflite: print only");
        open.info("Author", "Roelof Rossouw");
        open.info("Subject", "A PDF anyone can open and print; its other rights need the owner's password");
        open.info("Creator", "sc-pdflite-passwords");
        sc::pdflite::permissions allowed;
        allowed.print = true;
        allowed.copy = false;
        allowed.modify = false;
        SC_SHOW(allowed.bits()); // /P: what readers may do
        SC_STEP(open.protect({"", "admin", allowed})); // no user password: it opens without one
        SC_STEP(open.close());
        SC_SHOW(print_only.string());
        // [/readme]

        sc::console::output() << "\nAll of that took " << sw << ".\n";
    } catch (const std::exception &error) {
        std::cerr << "sc-pdflite-passwords: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
