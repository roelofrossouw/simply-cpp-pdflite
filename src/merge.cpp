#include "pdflite/merge.h"

#include "pdflite/file.h"

#include <memory>
#include <set>
#include <stdexcept>
#include <system_error>
#include <unordered_map>

namespace sc::pdflite {
    namespace {
        bool has_type(const object &value, const std::string_view type) {
            if (!value.is<dictionary>()) return false;
            const auto *found = value.as<dictionary>().find("Type");
            return found && found->is<name>() && found->as<name>().text() == type;
        }

        // One input's objects copied into the new file: each written once, under a new number, the
        // references between them renumbered.
        class copier {
        public:
            copier(const reader &in, file &out) : in_(in), out_(out) {
                // The input's own /Encrypt dictionary has no place in the new file.
                if (const auto *encrypt = in.trailer().find("Encrypt"); encrypt && encrypt->is<reference>()) dropped_.insert(encrypt->as<reference>().id);
            }

            // Each page under a new number, known before anything refers to it: links and
            // annotations name pages.
            std::vector<std::uint32_t> number_pages() {
                std::vector<std::uint32_t> out;
                for (const auto id: in_.pages()) {
                    const auto mine = out_.allocate();
                    pages_.try_emplace(id, mine); // a page that's in the tree twice: references go to the first
                    out.push_back(mine);
                }
                return out;
            }

            // Page index, written as number, under parent.
            void write_page(const std::size_t index, const std::uint32_t number, const std::uint32_t parent) {
                dictionary page;
                page.add("Type", name{"Page"});
                page.add("Parent", reference{parent});
                for (const auto &[key, value]: in_.page(index)) {
                    const auto &k = key.text();
                    // Its old parent, and what ties it to structures that aren't copied (article
                    // beads, the tagged structure tree).
                    if (k == "Type" || k == "Parent" || k == "B" || k == "StructParents") continue;
                    if (object copied = copy(value); !copied.is<std::nullptr_t>()) page.add(k, std::move(copied));
                }
                out_.write(number, page);
            }

            // value with its references renumbered, the objects they name queued to be written.
            object copy(const object &value) {
                if (value.is<reference>()) return copy_reference(value.as<reference>().id);
                if (value.is<array>()) {
                    array out;
                    for (const auto &item: value.as<array>()) out.push_back(copy(item));
                    return out;
                }
                if (value.is<dictionary>()) {
                    dictionary out;
                    for (const auto &[key, item]: value.as<dictionary>())
                        if (object copied = copy(item); !copied.is<std::nullptr_t>()) out.add(key.text(), std::move(copied));
                    return out;
                }
                return value;
            }

            // Writes every object queued (and those they refer to in turn).
            void flush() {
                while (!queue_.empty()) {
                    const auto [theirs, mine] = queue_.back();
                    queue_.pop_back();
                    if (auto data = in_.stream(theirs)) {
                        dictionary entries;
                        for (const auto &[key, item]: data->entries) {
                            if (key.text() == "Length") continue; // written for it
                            if (object copied = copy(item); !copied.is<std::nullptr_t>()) entries.add(key.text(), std::move(copied));
                        }
                        out_.write_stream(mine, std::move(entries), data->data, false); // still encoded as it was
                    } else {
                        out_.write(mine, copy(in_.get(theirs)));
                    }
                }
            }

        private:
            object copy_reference(const std::uint32_t theirs) {
                if (const auto page = pages_.find(theirs); page != pages_.end()) return reference{page->second};
                if (const auto done = copied_.find(theirs); done != copied_.end()) return reference{done->second};
                if (dropped_.contains(theirs)) return nullptr;
                const object target = in_.get(theirs);
                // A reference to nothing is a reference to null; the old page tree and catalog aren't
                // copied (their pages are, by number_pages()).
                if ((target.is<std::nullptr_t>() && !in_.is_stream(theirs)) || has_type(target, "Pages") || has_type(target, "Catalog")) {
                    dropped_.insert(theirs);
                    return nullptr;
                }
                const auto mine = out_.allocate();
                copied_.emplace(theirs, mine);
                queue_.emplace_back(theirs, mine);
                return reference{mine};
            }

            const reader &in_;
            file &out_;
            std::unordered_map<std::uint32_t, std::uint32_t> pages_;
            std::unordered_map<std::uint32_t, std::uint32_t> copied_;
            std::set<std::uint32_t> dropped_;
            std::vector<std::pair<std::uint32_t, std::uint32_t>> queue_;
        };
    }

    void merge(const std::vector<std::reference_wrapper<const reader>> &inputs, const std::filesystem::path &output,
               const std::optional<protection> &protect) {
        // Written beside output and renamed when done: output may be one of the inputs, and a
        // failure leaves it as it was.
        std::filesystem::path part = output;
        part += ".part";
        try {
            file out{part};
            std::uint32_t encrypt = 0;
            if (protect) {
                const auto security = std::make_shared<const encryption>(*protect);
                encrypt = out.allocate();
                out.encrypt(security, encrypt); // everything after it is encrypted, but its own dictionary
                out.write(encrypt, security->dictionary());
            }
            const auto catalog_id = out.allocate();
            const auto tree = out.allocate();
            array kids;
            dictionary catalog{{"Type", name{"Catalog"}}, {"Pages", reference{tree}}};
            std::uint32_t info = 0;
            for (std::size_t n = 0; n < inputs.size(); ++n) {
                const reader &in = inputs[n].get();
                copier copy{in, out};
                const auto numbers = copy.number_pages();
                for (std::size_t i = 0; i < numbers.size(); ++i) {
                    copy.write_page(i, numbers[i], tree);
                    kids.push_back(reference{numbers[i]});
                }
                if (n == 0) {
                    // The first input's information, bookmarks and named destinations.
                    if (const auto *found = in.trailer().find("Info")) {
                        if (const object copied = copy.copy(*found); copied.is<reference>()) {
                            info = copied.as<reference>().id;
                        } else if (copied.is<dictionary>()) {
                            info = out.allocate();
                            out.write(info, copied);
                        }
                    }
                    const dictionary theirs = in.catalog();
                    if (const auto *outlines = theirs.find("Outlines")) {
                        if (object copied = copy.copy(*outlines); !copied.is<std::nullptr_t>()) {
                            catalog.add("Outlines", std::move(copied));
                            if (const auto *mode = theirs.find("PageMode"); mode && mode->is<name>()) catalog.add("PageMode", *mode);
                        }
                    }
                    if (const auto *dests = theirs.find("Dests"))
                        if (object copied = copy.copy(*dests); !copied.is<std::nullptr_t>()) catalog.add("Dests", std::move(copied));
                    if (const auto *names = theirs.find("Names")) {
                        if (const object resolved = in.resolve(*names); resolved.is<dictionary>()) {
                            if (const auto *dests = resolved.as<dictionary>().find("Dests")) {
                                if (object copied = copy.copy(*dests); !copied.is<std::nullptr_t>())
                                    catalog.add("Names", dictionary{{"Dests", std::move(copied)}});
                            }
                        }
                    }
                    if (const auto *layout = theirs.find("PageLayout"); layout && layout->is<name>()) catalog.add("PageLayout", *layout);
                }
                copy.flush();
            }
            const auto count = kids.size();
            out.write(tree, dictionary{{"Type", name{"Pages"}}, {"Kids", std::move(kids)}, {"Count", count}});
            out.write(catalog_id, catalog);
            out.finish(catalog_id, info, encrypt);
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(part, ignored);
            throw;
        }
        std::error_code error;
        std::filesystem::rename(part, output, error);
        if (error) {
            std::filesystem::remove(part, error);
            throw std::runtime_error("sc::pdflite: can't write " + output.string());
        }
    }

    void merge(const std::vector<std::filesystem::path> &inputs, const std::filesystem::path &output, const std::optional<protection> &protect) {
        std::vector<reader> read;
        read.reserve(inputs.size());
        for (const auto &path: inputs) read.emplace_back(path);
        std::vector<std::reference_wrapper<const reader>> refs(read.begin(), read.end());
        merge(refs, output, protect);
    }

    void merge(const std::initializer_list<std::filesystem::path> inputs, const std::filesystem::path &output,
               const std::optional<protection> &protect) {
        merge(std::vector<std::filesystem::path>{inputs}, output, protect);
    }

    void protect(const std::filesystem::path &input, const std::filesystem::path &output, const protection &settings, const std::string_view password) {
        const reader in{input, password};
        merge({std::cref(in)}, output, settings);
    }
}
