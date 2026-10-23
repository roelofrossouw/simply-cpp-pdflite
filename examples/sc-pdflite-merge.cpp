// Merges PDFs with sc-pdflite: their pages, one file after another, in a new file, optionally with
// a background or stamp from another PDF on every page and passwords (AES-256). A JPEG or PNG among
// them becomes a page the size of the page before it. With --password, each input is read by
// sc::pdflite::reader and opened with it (pictures aren't taken then).
//
//     sc-pdflite-merge report.pdf appendix-a.pdf appendix-b.pdf --output all.pdf
//     sc-pdflite-merge report.pdf photo.jpg scan.png --output all.pdf
//     sc-pdflite-merge report.pdf --background letterhead.pdf --output letter.pdf
//     sc-pdflite-merge report.pdf --output locked.pdf --user-password "open sesame" --owner-password admin

#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include <params.h>

#include <pdflite.h>

int main(int argc, char **argv) {
    const sc::params args{argc, argv, {
        {"name", "sc-pdflite-merge"},
        {"description", "Writes the pages of PDFs, one file after another, into a new file (the first one's title, "
                        "bookmarks and named destinations with them), optionally with another PDF's pages behind or over "
                        "them (its page n on page n, its last on the rest) and passwords (AES-256). A JPEG or PNG becomes a "
                        "page the size of the page before it, turned upright, fitted and centred. The output may be one of "
                        "the inputs."},
        {"options", {
            {"output", {{"short", "o"}, {"value", "FILE"}, {"required", true}, {"help", "the PDF to write"}}},
            {"password", {{"short", "p"}, {"value", "TEXT"}, {"default", ""}, {"help", "opens the inputs that are encrypted (PDFs only then)"}}},
            {"background", {{"short", "b"}, {"value", "PDF"}, {"default", ""}, {"help", "draw its pages behind the pages"}}},
            {"stamp", {{"short", "s"}, {"value", "PDF"}, {"default", ""}, {"help", "draw its pages over the pages"}}},
            {"user-password", {{"value", "TEXT"}, {"default", ""}, {"help", "a password to open the output"}}},
            {"owner-password", {{"value", "TEXT"}, {"default", ""},
                                {"help", "a password for every right to the output (without --user-password, anyone opens "
                                         "it and may only print)"}}}}},
        {"extra", {
            {"inputs", {{"required", true}, {"help", "the PDFs (and JPEGs and PNGs), in order"}}}}}}};

    try {
        // [readme]
        std::vector<std::filesystem::path> files;
        for (const auto &file: args["inputs"]) files.emplace_back(file.get<std::string>());
        std::optional<sc::pdflite::protection> protection;
        const auto user = args.as<std::string>("user-password"), owner = args.as<std::string>("owner-password");
        if (!user.empty() || !owner.empty()) protection = sc::pdflite::protection{user, owner};
        const std::filesystem::path output = args.as<std::string>("output");
        // The files merged, pictures made pages; or, with a password, each PDF opened with it.
        auto merge = [&](const std::optional<sc::pdflite::protection> &with) {
            const auto password = args.as<std::string>("password");
            if (password.empty()) return sc::pdflite::merge(files, output, with);
            std::vector<sc::pdflite::reader> opened;
            for (const auto &file: files) opened.emplace_back(file, password);
            sc::pdflite::merge(std::vector<std::reference_wrapper<const sc::pdflite::reader>>(opened.begin(), opened.end()), output, with);
        };
        const auto background = args.as<std::string>("background"), stamp = args.as<std::string>("stamp");
        if (background.empty() && stamp.empty()) {
            merge(protection);
        } else {
            // Merged first, then decorated in place: the passwords go on last.
            merge(std::nullopt);
            if (!background.empty())
                sc::pdflite::decorate(output, background, output, sc::pdflite::layer::behind, stamp.empty() ? protection : std::nullopt);
            if (!stamp.empty()) sc::pdflite::decorate(output, stamp, output, sc::pdflite::layer::over, protection);
        }
        std::cout << output.string() << ": " << sc::pdflite::reader{output, user.empty() ? owner : user}.page_count() << " pages"
                  << (protection ? " (encrypted)" : "") << '\n';
        // [/readme]
    } catch (const std::exception &error) {
        std::cerr << "sc-pdflite-merge: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
