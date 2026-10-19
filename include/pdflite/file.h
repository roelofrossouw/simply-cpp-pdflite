#ifndef SC_PDFLITE_FILE_H
#define SC_PDFLITE_FILE_H

#include "encryption.h"
#include "object.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

// sc-pdflite's file (ISO 32000-1, 7.5): the header, the objects, the cross-reference table and the trailer.
namespace sc::pdflite {
    // The file: a header, objects in any order (each written once, by the number allocate() gave
    // it), and on finish() the cross-reference table and trailer. Objects go to the file as they're
    // written; nothing is held but their offsets.
    class file {
    public:
        // Throws std::runtime_error when the file can't be created.
        explicit file(const std::filesystem::path &path);

        // The next object number, to write later (and refer to before then).
        std::uint32_t allocate();

        // Writes indirect object id (7.3.10): "id 0 obj", value, "endobj". Each allocated object
        // is written once; throws std::logic_error otherwise.
        void write(std::uint32_t id, const pdflite::object &value);

        // Writes object id as a stream (7.3.8): its dictionary, which gets /Length, then data.
        // Unless compress is false, data is compressed with zlib and the dictionary gets /Filter
        // /FlateDecode. Throws std::invalid_argument when the dictionary has /Length already, or a
        // /Filter of its own with compress (data that's encoded already, such as a JPEG with
        // /DCTDecode, is written with compress false).
        void write_stream(std::uint32_t id, dictionary entries, std::string_view data, bool compress = true);

        // From now on, encrypts every string and stream written (7.6), but object exempt's: the
        // /Encrypt dictionary, which is read before anything can be decrypted.
        void encrypt(std::shared_ptr<const encryption> with, std::uint32_t exempt);

        // Writes the cross-reference table and the trailer, naming the catalog (root), the
        // information dictionary (info, 0: none) and the encryption dictionary (encrypt, 0: none),
        // and closes the file. Throws std::logic_error when an allocated object wasn't written, and
        // std::runtime_error when writing failed.
        void finish(std::uint32_t root, std::uint32_t info, std::uint32_t encrypt = 0);

        [[nodiscard]] bool finished() const { return finished_; }

    private:
        void put(std::string_view bytes);

        // value with its strings encrypted, when they're to be (object id).
        [[nodiscard]] pdflite::object sealed(const pdflite::object &value, std::uint32_t id) const;

        std::shared_ptr<const encryption> encryption_;
        std::uint32_t exempt_ = 0;

        // Starts object id: checks it may be written and notes where it begins.
        void start(std::uint32_t id);

        std::filesystem::path path_;
        std::ofstream out_;
        std::uint64_t offset_ = 0;
        std::vector<std::uint64_t> offsets_; // by object number - 1; 0 until written
        bool finished_ = false;
    };

    // zlib's deflate of data, as FlateDecode reads it.
    std::string deflate(std::string_view data);
}

#endif //SC_PDFLITE_FILE_H
