#ifndef SC_PDFLITE_READER_H
#define SC_PDFLITE_READER_H

#include "object.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// sc-pdflite's reader: an existing PDF's objects and pages (ISO 32000-1, 7.5), for copying pages
// out of it (pdflite/merge.h). It reads well-formed files: cross-reference tables and streams,
// object streams, incremental updates (the newest object wins), FlateDecode (with its
// predictors), and files encrypted by the standard security handler (RC4 and AES-128, revisions 2
// to 4; AES-256, revision 6). A damaged file is repaired as it's read, as qpdf and MuPDF repair
// one: when its cross-reference doesn't lead to its objects, it's rebuilt by scanning the file
// for them (repaired() says so), and when its page tree can't be read, its pages are taken in the
// order they're in the file.
namespace sc::pdflite {
    // A stream as the file holds it: its dictionary and its data, decrypted but still encoded by
    // its filters (/Filter), so it can be written again as it is.
    struct stream {
        pdflite::dictionary entries;
        std::string data;
    };

    class reader {
    public:
        // Reads a file, and opens it with password (the user's or the owner's) when it's encrypted;
        // an empty password opens a file that needs none to be read (an owner password only).
        // Throws std::runtime_error when it can't be read, isn't a PDF, is damaged beyond repair (no
        // catalog or pages can be found), uses a security handler other than the standard one, or
        // the password doesn't open it.
        explicit reader(const std::filesystem::path &file, std::string_view password = {});

        // The same, from a PDF in memory.
        [[nodiscard]] static reader from_memory(std::string bytes, std::string_view password = {});

        // The (newest) trailer: /Root, /Info, /ID ...
        [[nodiscard]] const pdflite::dictionary &trailer() const;

        // Whether it was encrypted (what's read from it is decrypted).
        [[nodiscard]] bool encrypted() const;

        // Whether its cross-reference had to be rebuilt, by scanning the file for its objects, as
        // a damaged file's is (where it was cut short, offsets that don't lead to their objects, a
        // table that can't be read).
        [[nodiscard]] bool repaired() const;

        // Object id's value, a stream's dictionary for a stream; null for one that's free or not in
        // the file (7.3.10: a reference to it is a reference to null). Throws std::runtime_error when
        // the object can't be read.
        [[nodiscard]] pdflite::object get(std::uint32_t id) const;

        // Whether object id is a stream, and the stream.
        [[nodiscard]] bool is_stream(std::uint32_t id) const;

        [[nodiscard]] std::optional<pdflite::stream> stream(std::uint32_t id) const;

        // value, or what it refers to when it's a reference.
        [[nodiscard]] pdflite::object resolve(const pdflite::object &value) const;

        // A stream's data with its filters undone. FlateDecode (with PNG and TIFF predictors) is what
        // the reader needs and all it decodes: throws std::runtime_error for other filters.
        [[nodiscard]] std::string decoded(const pdflite::stream &encoded) const;

        // The pages' object numbers, in order (7.7.3).
        [[nodiscard]] const std::vector<std::uint32_t> &pages() const;

        [[nodiscard]] std::size_t page_count() const { return pages().size(); }

        // Page index's dictionary, with what it inherits from the page tree (/Resources, /MediaBox,
        // /CropBox, /Rotate; 7.7.3.4) filled in. Throws std::out_of_range for a page it hasn't.
        [[nodiscard]] pdflite::dictionary page(std::size_t index) const;

        // The catalog (/Root).
        [[nodiscard]] pdflite::dictionary catalog() const;

    private:
        struct impl;

        explicit reader(std::shared_ptr<impl> state);

        static std::shared_ptr<impl> opened(std::string bytes, std::string_view password);

        std::shared_ptr<impl> impl_;
    };
}

#endif //SC_PDFLITE_READER_H
