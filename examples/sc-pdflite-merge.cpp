// Merges PDFs with sc-pdflite: their pages, one file after another, in a new file, optionally with
// passwords (AES-256). Each input is read by sc::pdflite::reader, and opened with --password when
// it's encrypted.
//
//     sc-pdflite-merge report.pdf appendix-a.pdf appendix-b.pdf --output all.pdf
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
                        "bookmarks and named destinations with them), optionally with passwords (AES-256). The output "
                        "may be one of the inputs."},
        {"options", {
            {"output", {{"short", "o"}, {"value", "FILE"}, {"required", true}, {"help", "the PDF to write"}}},
            {"password", {{"short", "p"}, {"value", "TEXT"}, {"default", ""}, {"help", "opens the inputs that are encrypted"}}},
            {"user-password", {{"value", "TEXT"}, {"default", ""}, {"help", "a password to open the output"}}},
            {"owner-password", {{"value", "TEXT"}, {"default", ""},
                                {"help", "a password for every right to the output (without --user-password, anyone opens "
                                         "it and may only print)"}}}}},
        {"extra", {
            {"inputs", {{"required", true}, {"help", "the PDFs, in order"}}}}}}};

    try {
        // [readme]
        std::vector<sc::pdflite::reader> inputs;
        for (const auto &file: args["inputs"]) {
            inputs.emplace_back(file.get<std::string>(), args.as<std::string>("password"));
            std::cout << file.get<std::string>() << ": " << inputs.back().page_count() << " pages"
                      << (inputs.back().encrypted() ? " (encrypted)" : "") << '\n';
        }
        std::optional<sc::pdflite::protection> protection;
        const auto user = args.as<std::string>("user-password"), owner = args.as<std::string>("owner-password");
        if (!user.empty() || !owner.empty()) protection = sc::pdflite::protection{user, owner};
        const std::filesystem::path output = args.as<std::string>("output");
        const std::vector<std::reference_wrapper<const sc::pdflite::reader>> files(inputs.begin(), inputs.end());
        sc::pdflite::merge(files, output, protection);
        std::cout << output.string() << ": " << sc::pdflite::reader{output, user.empty() ? owner : user}.page_count() << " pages"
                  << (protection ? " (encrypted)" : "") << '\n';
        // [/readme]
    } catch (const std::exception &error) {
        std::cerr << "sc-pdflite-merge: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
