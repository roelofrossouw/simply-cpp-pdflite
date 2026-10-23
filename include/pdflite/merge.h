#ifndef SC_PDFLITE_MERGE_H
#define SC_PDFLITE_MERGE_H

#include "encryption.h"
#include "reader.h"

#include <filesystem>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <vector>

// Existing PDFs written again (pdflite/reader.h): their pages one after another in a new file, and
// a file given passwords.
namespace sc::pdflite {
    // Writes the inputs' pages, in order, into a new file at output (replacing it; it may be one of
    // the inputs). Each page comes as it was: its content, resources, annotations, and links to
    // pages of its own file. The first input's document information (title, author ...),
    // bookmarks and named destinations come too; nothing else of the others (their bookmarks,
    // form fields, tagged structure) does. protect gives the new file passwords (AES-256).
    // Throws std::runtime_error when the file can't be written (output is then left as it was).
    void merge(const std::vector<std::reference_wrapper<const reader>> &inputs, const std::filesystem::path &output,
               const std::optional<protection> &protect = std::nullopt);

    // The same for files, each read as reader reads it without a password (so a file that needs
    // one to be opened throws std::runtime_error).
    void merge(const std::vector<std::filesystem::path> &inputs, const std::filesystem::path &output,
               const std::optional<protection> &protect = std::nullopt);

    // The same, the files listed in place: merge({"report.pdf", "appendix.pdf"}, "report.pdf").
    void merge(std::initializer_list<std::filesystem::path> inputs, const std::filesystem::path &output,
               const std::optional<protection> &protect = std::nullopt);

    // input written again at output (which may be input) with passwords (AES-256); password opens
    // input when it's encrypted already.
    void protect(const std::filesystem::path &input, const std::filesystem::path &output, const protection &settings,
                 std::string_view password = {});
}

#endif //SC_PDFLITE_MERGE_H
