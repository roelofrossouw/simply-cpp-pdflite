#ifndef SC_PDFLITE_RESOURCE_H
#define SC_PDFLITE_RESOURCE_H

#include "file.h"

#include <cstdint>
#include <string_view>

namespace sc::pdflite {
    // Something a page's content uses by name (ISO 32000-1, 7.8.3): an image or a font, written
    // into the file once however many pages use it. Each kind derives from it and writes itself.
    class resource {
    public:
        virtual ~resource() = default;

        // The prefix of its names in a page's /Resources: "Im" makes /Im1, /Im2, ...
        [[nodiscard]] virtual std::string_view prefix() const = 0;

        virtual void write(file &out, std::uint32_t id) const = 0;
    };
}

#endif //SC_PDFLITE_RESOURCE_H
