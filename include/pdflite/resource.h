#ifndef SC_PDFLITE_RESOURCE_H
#define SC_PDFLITE_RESOURCE_H

#include "file.h"

#include <cstdint>
#include <string_view>

// sc-pdflite's resources (ISO 32000-1, 7.8.3): what a page's content refers to by name, written
// into the file once however many pages use it.
namespace sc::pdflite {
    // Something a page uses by name: an image, and later fonts and graphics states. Each kind
    // derives from it and writes itself.
    class resource {
    public:
        virtual ~resource() = default;

        // The prefix of its names there: "Im" makes /Im1, /Im2, ...
        [[nodiscard]] virtual std::string_view prefix() const = 0;

        // Writes it into out as object id.
        virtual void write(file &out, std::uint32_t id) const = 0;
    };
}

#endif //SC_PDFLITE_RESOURCE_H
