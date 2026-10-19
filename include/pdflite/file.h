#ifndef SC_PDFLITE_FILE_H
#define SC_PDFLITE_FILE_H

#include <cstdint>
#include <filesystem>
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

        // Writes object id: "id 0 obj", body, "endobj".
        void object(std::uint32_t id, std::string_view body);

        // Writes object id as a stream: dictionary holds its entries (without /Length or /Filter),
        // and data is compressed with zlib (FlateDecode) when compress is set.
        void stream(std::uint32_t id, std::string_view dictionary, std::string_view data, bool compress = true);

        // Writes the cross-reference table and the trailer, naming root and info (0: none), and
        // closes the file. Throws std::logic_error when an allocated object wasn't written, and
        // std::runtime_error when writing failed.
        void finish(std::uint32_t root, std::uint32_t info);

        [[nodiscard]] bool finished() const { return finished_; }

    private:
        void write(std::string_view bytes);

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
