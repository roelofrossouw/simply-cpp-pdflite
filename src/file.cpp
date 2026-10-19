#include "pdflite/file.h"

#include "pdflite/object.h"

#include <zlib.h>

#include <cstdio>
#include <random>
#include <stdexcept>
#include <string>

namespace sc::pdflite {
    std::string deflate(const std::string_view data) {
        uLongf size = compressBound(static_cast<uLong>(data.size()));
        std::string out(size, '\0');
        if (compress2(reinterpret_cast<Bytef *>(out.data()), &size, reinterpret_cast<const Bytef *>(data.data()),
                      static_cast<uLong>(data.size()), Z_DEFAULT_COMPRESSION) != Z_OK)
            throw std::runtime_error("sc::pdflite: zlib couldn't compress a stream");
        out.resize(size);
        return out;
    }

    file::file(const std::filesystem::path &path) : path_(path), out_(path, std::ios::binary | std::ios::trunc) {
        if (!out_) throw std::runtime_error("sc::pdflite: can't create " + path.string());
        // The version, then a comment of bytes above 127, so transfers treat the file as binary (7.5.2).
        put("%PDF-1.7\n%\xE2\xE3\xCF\xD3\n");
    }

    std::uint32_t file::allocate() {
        offsets_.push_back(0);
        return static_cast<std::uint32_t>(offsets_.size());
    }

    void file::put(const std::string_view bytes) {
        out_.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        offset_ += bytes.size();
    }

    void file::start(const std::uint32_t id) {
        if (finished_) throw std::logic_error("sc::pdflite: writing an object after the file was finished");
        if (id == 0 || id > offsets_.size()) throw std::logic_error("sc::pdflite: an object number that wasn't allocated");
        if (offsets_[id - 1] != 0) throw std::logic_error("sc::pdflite: object " + std::to_string(id) + " written twice");
        offsets_[id - 1] = offset_;
        put(std::to_string(id) + " 0 obj\n");
    }

    void file::write(const std::uint32_t id, const pdflite::object &value) {
        const std::string text = value.write(); // before start(): a value that can't be written leaves no trace
        start(id);
        put(text);
        put("\nendobj\n");
    }

    void file::write_stream(const std::uint32_t id, dictionary entries, const std::string_view data, const bool compress) {
        if (entries.contains("Length")) throw std::invalid_argument("sc::pdflite: a stream's /Length is written for it");
        if (compress && entries.contains("Filter"))
            throw std::invalid_argument("sc::pdflite: a compressed stream's /Filter is written for it");
        const std::string packed = compress ? deflate(data) : std::string{data};
        entries.add("Length", packed.size());
        if (compress) entries.add("Filter", name{"FlateDecode"});
        const std::string head = entries.write();
        start(id);
        put(head);
        put("\nstream\n"); // the end of line after "stream" is a line feed, never a lone return
        put(packed);
        put("\nendstream\nendobj\n");
    }

    void file::finish(const std::uint32_t root, const std::uint32_t info) {
        if (finished_) return;
        for (std::size_t i = 0; i < offsets_.size(); ++i)
            if (offsets_[i] == 0) throw std::logic_error("sc::pdflite: object " + std::to_string(i + 1) + " was never written");
        const std::uint64_t table = offset_;
        // Every entry is exactly 20 bytes, ending in a space and a line feed (7.5.4).
        std::string xref = "xref\n0 " + std::to_string(offsets_.size() + 1) + "\n0000000000 65535 f \n";
        for (const auto offset: offsets_) {
            char entry[21];
            std::snprintf(entry, sizeof entry, "%010llu 00000 n \n", static_cast<unsigned long long>(offset));
            xref += entry;
        }
        put(xref);
        // The file's identifier (14.4), random: both halves the same, as in a file never updated.
        std::random_device random;
        std::string id;
        for (int i = 0; i < 16; ++i) id += static_cast<char>(random() & 0xFF);
        dictionary trailer{{"Size", offsets_.size() + 1}, {"Root", reference{root}}};
        if (info != 0) trailer.add("Info", reference{info});
        trailer.add("ID", array{string{id, true}, string{id, true}});
        put("trailer\n" + trailer.write() + "\nstartxref\n" + std::to_string(table) + "\n%%EOF\n");
        out_.close();
        finished_ = true;
        if (!out_) throw std::runtime_error("sc::pdflite: writing " + path_.string() + " failed");
    }
}
