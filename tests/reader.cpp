// sc-pdflite's reader, merge() and protect(): files written by pdflite itself and by others
// (tests/resource/pdf: qpdf, macOS's Quartz, MuPDF), cross-reference tables and streams, object
// streams, linearized files, incremental updates, and every encryption the standard security
// handler has (RC4 40 and 128 bits, AES-128 and AES-256); merges and protected files are read back
// page by page. The files written go to SC_PDFLITE_OUTPUT too, for ctest's check-* tests.
#include "pdflite.h"

#include "sc_test.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace sc::pdflite;

namespace {
    fs::path output() {
        const char *where = std::getenv("SC_PDFLITE_OUTPUT");
        fs::path out = where && *where ? fs::path{where} : fs::temp_directory_path() / "sc-pdflite";
        fs::create_directories(out);
        return out;
    }

    const fs::path fixtures{SC_PDFLITE_TEST_PDFS};

    std::string read(const fs::path &path) {
        std::ifstream in{path, std::ios::binary};
        return {std::istreambuf_iterator<char>{in}, {}};
    }

    // A page's content, its streams decoded one after another.
    std::string page_text(const reader &pdf, const std::size_t index) {
        const auto page = pdf.page(index);
        const auto *contents = page.find("Contents");
        if (!contents) return {};
        std::vector<object> parts;
        // A reference to a stream, or an array of them (itself perhaps a reference).
        const bool one = contents->is<reference>() && pdf.is_stream(contents->as<reference>().id);
        if (const object resolved = one ? *contents : pdf.resolve(*contents); resolved.is<array>()) {
            for (const auto &part: resolved.as<array>()) parts.push_back(part);
        } else {
            parts.push_back(resolved);
        }
        std::string out;
        for (const auto &part: parts)
            if (part.is<reference>())
                if (const auto data = pdf.stream(part.as<reference>().id)) out += pdf.decoded(*data);
        return out;
    }

    // The password ctest's checkers open a file with, beside it (check_pdfs.cmake).
    void note_password(const fs::path &pdf, const std::string &password) { std::ofstream{fs::path{pdf} += ".password"} << password; }

    std::string text_entry(const dictionary &entries, const std::string_view key) {
        const auto *found = entries.find(key);
        return found && found->is<string>() ? found->as<string>().bytes() : std::string{};
    }

    std::string title(const reader &pdf) {
        const auto *info = pdf.trailer().find("Info");
        if (!info) return {};
        const object resolved = pdf.resolve(*info);
        return resolved.is<dictionary>() ? text_entry(resolved.as<dictionary>(), "Title") : std::string{};
    }

    // A 3-page PDF of pdflite's own: a word on each page, a link from page 1 to page 3, a bookmark.
    fs::path own(const fs::path &where, const std::string &word, const std::optional<protection> &settings = std::nullopt) {
        document pdf{where, sc::page_size::A5};
        pdf.add_page();
        pdf.add_page();
        for (std::size_t page = 1; page <= 3; ++page) {
            pdf.set_page(page);
            const auto font = pdf.standard_font(sc::standard_font::face::helvetica); // in each page's resources
            auto &c = pdf.page_content();
            c.begin_text();
            c.font(font, 12);
            c.text_position(72, 72);
            c.show(word + " " + std::to_string(page));
            c.end_text();
        }
        pdf.set_page(1);
        pdf.link(sc::rect{72, 100, 100, 20}, 3);
        pdf.bookmark("The third page", 3);
        pdf.info("Title", word);
        if (settings) pdf.protect(*settings);
        pdf.close();
        return where;
    }
}

int main() {
    using std::string;
    const auto out = output();

    SECTION("pdflite's own file: pages, inherited attributes, content, trailer");
    {
        const reader pdf{own(out / "reader-own.pdf", "Alpha")};
        CHECK_EQ(pdf.page_count(), std::size_t{3});
        CHECK(!pdf.encrypted());
        CHECK(pdf.page(0).contains("MediaBox"));
        CHECK(pdf.page(2).contains("Resources"));
        CHECK(page_text(pdf, 0).find("(Alpha 1) Tj") != string::npos);
        CHECK(page_text(pdf, 2).find("(Alpha 3) Tj") != string::npos);
        CHECK_EQ(title(pdf), string{"Alpha"});
        CHECK(pdf.catalog().contains("Outlines"));
        CHECK_THROWS_AS(pdf.page(3), std::out_of_range);
        CHECK(pdf.get(1'000'000).is<std::nullptr_t>()); // an object that isn't there is null
        const reader again = reader::from_memory(read(out / "reader-own.pdf"));
        CHECK_EQ(again.page_count(), std::size_t{3});
    }

    SECTION("Files by others: cross-reference streams, object streams, linearized, Quartz, MuPDF");
    {
        for (const auto &[file, pages]: std::vector<std::pair<string, std::size_t>>{{"objstm.pdf", 7}, {"linearized.pdf", 7}, {"quartz.pdf", 1}, {"mutool.pdf", 1}}) {
            const reader pdf{fixtures / file};
            CHECK_EQ(pdf.page_count(), pages);
            CHECK(!page_text(pdf, 0).empty());
            CHECK(pdf.page(pages - 1).contains("MediaBox"));
        }
        CHECK_EQ(title(reader{fixtures / "objstm.pdf"}), string{"Check"});
        CHECK(page_text(reader{fixtures / "mutool.pdf"}, 0).find("(Made by mutool create) Tj") != string::npos);
    }

    SECTION("Encrypted files: RC4 40 and 128 bits, AES-128 and AES-256, by user or owner password");
    {
        const string plain = page_text(reader{fixtures / "objstm.pdf"}, 0);
        for (const auto &[file, password]: std::vector<std::pair<string, string>>{
                 {"rc4-40.pdf", ""}, {"rc4-128.pdf", "user"}, {"rc4-128.pdf", "owner"}, {"aes-128-owner.pdf", ""},
                 {"aes-128-owner.pdf", "owner"}, {"aes-256-objstm.pdf", "user"}, {"aes-256-objstm.pdf", "owner"}}) {
            const reader pdf{fixtures / file, password};
            CHECK(pdf.encrypted());
            CHECK_EQ(pdf.page_count(), std::size_t{7});
            const string first = page_text(pdf, 0); // a copy: CHECK_EQ keeps a reference
            CHECK_EQ(first, plain);
            CHECK_EQ(title(pdf), string{"Check"}); // strings decrypted too
        }
        CHECK_THROWS_AS(reader(fixtures / "rc4-128.pdf"), std::runtime_error);         // a user password needed
        CHECK_THROWS_AS(reader(fixtures / "aes-256-objstm.pdf", "wrong"), std::runtime_error);
        const reader own_locked{own(out / "reader-locked.pdf", "Beta", protection{"open sesame", "admin"}), "open sesame"};
        note_password(out / "reader-locked.pdf", "open sesame");
        CHECK(page_text(own_locked, 1).find("(Beta 2) Tj") != string::npos);
        CHECK_THROWS_AS(reader(out / "reader-locked.pdf"), std::runtime_error);
    }

    SECTION("An incremental update: the newest object and trailer win");
    {
        const auto original = read(own(out / "reader-update-base.pdf", "Gamma"));
        const reader before = reader::from_memory(original);
        const auto previous = original.substr(original.rfind("startxref\n") + 10);
        const auto root = before.trailer().at("Root").as<reference>().id;
        const auto size = static_cast<std::uint32_t>(before.trailer().at("Size").as<std::int64_t>());
        // A new information dictionary, as an editor appends one.
        string updated = original;
        const auto at = updated.size();
        updated += std::to_string(size) + " 0 obj\n<< /Title (Gamma, updated) >>\nendobj\n";
        const auto table = updated.size();
        char entry[32];
        std::snprintf(entry, sizeof entry, "%010zu 00000 n \n", at);
        updated += "xref\n" + std::to_string(size) + " 1\n" + entry + "trailer\n<< /Size " + std::to_string(size + 1) + " /Root " +
                   std::to_string(root) + " 0 R /Info " + std::to_string(size) + " 0 R /Prev " + std::to_string(std::stoull(previous)) +
                   " >>\nstartxref\n" + std::to_string(table) + "\n%%EOF\n";
        const reader after = reader::from_memory(updated);
        CHECK_EQ(title(after), string{"Gamma, updated"});
        CHECK_EQ(after.page_count(), std::size_t{3});
    }

    SECTION("Not a PDF, or cut short: std::runtime_error");
    {
        CHECK_THROWS_AS(reader::from_memory("Hello, world"), std::runtime_error);
        const auto whole = read(fixtures / "quartz.pdf");
        CHECK_THROWS_AS(reader::from_memory(whole.substr(0, whole.size() / 2)), std::runtime_error);
        CHECK_THROWS_AS(reader(out / "no-such-file.pdf"), std::runtime_error);
        const reader pdf{fixtures / "quartz.pdf"};
        CHECK_THROWS_AS(pdf.decoded(sc::pdflite::stream{dictionary{{"Filter", name{"DCTDecode"}}}, "x"}), std::runtime_error);
    }

    SECTION("merge(): every page in order, links and the first file's title and bookmarks kept");
    {
        const auto a = own(out / "reader-a.pdf", "Alpha");
        const auto merged = out / "reader-merged.pdf";
        merge({a, fixtures / "objstm.pdf", fixtures / "quartz.pdf", fixtures / "rc4-40.pdf", fixtures / "aes-128-owner.pdf"}, merged);
        const reader pdf{merged};
        CHECK_EQ(pdf.page_count(), std::size_t{3 + 7 + 1 + 7 + 7});
        CHECK(page_text(pdf, 0).find("(Alpha 1) Tj") != string::npos);
        CHECK(page_text(pdf, 2).find("(Alpha 3) Tj") != string::npos);
        const string appendix = page_text(pdf, 3), report = page_text(reader{fixtures / "objstm.pdf"}, 0); // copies for CHECK_EQ
        CHECK_EQ(appendix, report);
        const string decrypted = page_text(pdf, 11), original = page_text(reader{fixtures / "rc4-40.pdf"}, 0);
        CHECK_EQ(decrypted, original);
        CHECK_EQ(title(pdf), string{"Alpha"});
        CHECK(pdf.catalog().contains("Outlines"));
        // Page 1's link still goes to page 3: a reference to that page's new object.
        const auto annots = pdf.resolve(pdf.page(0).at("Annots")).as<array>();
        CHECK_EQ(annots.size(), std::size_t{1});
        if (annots.size() == 1) {
            const auto link = pdf.resolve(annots[0]).as<dictionary>();
            const auto *dest = link.find("Dest");
            const bool to_page_three = dest && dest->is<array>() && dest->as<array>()[0].is<reference>() &&
                                       dest->as<array>()[0].as<reference>().id == pdf.pages()[2];
            CHECK(to_page_three);
        }
    }

    SECTION("merge() into one of its inputs, and protect(): AES-256, read back with the password");
    {
        const auto report = own(out / "reader-in-place.pdf", "Delta");
        merge({report, fixtures / "mutool.pdf"}, report); // as MergePDF($file1, $file2) does
        const auto copy = (out / "reader-two-names.pdf").string();
        fs::copy_file(fixtures / "quartz.pdf", copy, fs::copy_options::overwrite_existing);
        // Two names in place (an initializer list, not an iterator pair); the second isn't there.
        const auto two_names = [&] { merge({copy.c_str(), "/dev/null/none.pdf"}, out / "never.pdf"); };
        CHECK_THROWS_AS(two_names(), std::runtime_error);
        CHECK(!fs::exists(out / "never.pdf"));
        CHECK_EQ(reader{report}.page_count(), std::size_t{4});
        CHECK(!fs::exists(fs::path{report}.concat(".part")));
        const auto locked = out / "reader-protected.pdf";
        protect(report, locked, protection{"open", "admin"});
        note_password(locked, "open");
        CHECK_THROWS_AS(reader(locked), std::runtime_error);
        const reader pdf{locked, "open"};
        CHECK(pdf.encrypted());
        CHECK_EQ(pdf.page_count(), std::size_t{4});
        CHECK(page_text(pdf, 3).find("(Made by mutool create) Tj") != string::npos);
        CHECK_EQ(title(pdf), string{"Delta"});
        const reader by_owner{locked, "admin"};
        CHECK_EQ(by_owner.page_count(), std::size_t{4});
        // An encrypted input opened by its password, protected again.
        const auto again = out / "reader-protected-again.pdf";
        protect(fixtures / "rc4-128.pdf", again, protection{"", "admin"}, "user");
        CHECK_EQ(reader{again}.page_count(), std::size_t{7}); // anyone opens it: an owner password only
        // A failure leaves the output as it was.
        const auto before = read(locked);
        CHECK_THROWS_AS(merge(std::vector<fs::path>{fixtures / "rc4-128.pdf"}, locked), std::runtime_error);
        CHECK(read(locked) == before);
    }

    SECTION("decorate(): a page behind every page, or over it; each decoration page a form written once");
    {
        // A three-page decoration of pdflite's own (A5).
        const auto letterhead = own(out / "reader-letterhead.pdf", "Letterhead");
        const auto report = fixtures / "objstm.pdf"; // 7 pages
        const auto behind = out / "reader-behind.pdf";
        decorate(report, letterhead, behind);
        const reader pdf{behind};
        CHECK_EQ(pdf.page_count(), std::size_t{7});
        CHECK_EQ(title(pdf), string{"Check"}); // the input's information, not the decoration's
        // Each page names the form; the letterhead's three pages are 1, 2, and 3 on every later page.
        auto form_of = [&](const reader &r, const std::size_t page) -> std::uint32_t {
            const object resources = r.resolve(r.page(page).at("Resources"));
            const object forms = r.resolve(resources.as<dictionary>().at("XObject"));
            const auto *found = forms.as<dictionary>().find("SCDecoration");
            return found && found->is<reference>() ? found->as<reference>().id : 0;
        };
        CHECK(form_of(pdf, 0) != 0);
        CHECK(form_of(pdf, 0) != form_of(pdf, 1));
        CHECK(form_of(pdf, 2) != form_of(pdf, 1));
        CHECK_EQ(form_of(pdf, 6), form_of(pdf, 2)); // its last page on the rest, written once
        const auto form = pdf.stream(form_of(pdf, 0));
        CHECK(form.has_value());
        if (form) {
            CHECK(pdf.decoded(*form).find("(Letterhead 1) Tj") != string::npos);
            const auto subtype = form->entries.at("Subtype").as<name>().text(); // a copy for CHECK_EQ
            CHECK_EQ(subtype, string{"Form"});
        }
        // Behind: drawn first, the page's own content after it, unchanged.
        const string first = page_text(pdf, 0);
        CHECK(first.starts_with("q 1.4143 0 0 1.4143 ")); // the A5 letterhead scaled up to the A4 page
        CHECK(first.find("/SCDecoration Do Q") < first.find("BT"));
        CHECK(first.ends_with(page_text(reader{report}, 0)));

        // Over: the page's content kept to itself in q ... Q, the stamp drawn after it.
        const auto over = out / "reader-over.pdf";
        decorate(report, letterhead, over, layer::over);
        const string stamped = page_text(reader{over}, 0);
        CHECK(stamped.starts_with("q\n"));
        CHECK(stamped.find("/SCDecoration Do Q") > stamped.find(page_text(reader{report}, 0)));

        // A page turned (/Rotate 90): the decoration turned with it, so it's upright as shown.
        const auto turned = out / "reader-turned.pdf";
        const auto rotated = read(fixtures / "quartz.pdf");
        const reader quartz = reader::from_memory(rotated);
        const auto page_id = quartz.pages()[0];
        // An incremental update giving the page /Rotate 90.
        dictionary page = quartz.get(page_id).as<dictionary>();
        page.set("Rotate", 90);
        string updated = rotated;
        const auto at = updated.size();
        updated += std::to_string(page_id) + " 0 obj\n" + page.write() + "\nendobj\n";
        const auto table = updated.size();
        char entry[32];
        std::snprintf(entry, sizeof entry, "%010zu 00000 n \n", at);
        const auto previous = rotated.substr(rotated.rfind("startxref") + 9);
        updated += "xref\n" + std::to_string(page_id) + " 1\n" + entry + "trailer\n<< /Size " +
                   std::to_string(quartz.trailer().at("Size").as<std::int64_t>()) + " /Root " +
                   std::to_string(quartz.trailer().at("Root").as<reference>().id) + " 0 R /Prev " + std::to_string(std::stoull(previous)) +
                   " >>\nstartxref\n" + std::to_string(table) + "\n%%EOF\n";
        const auto turned_input = out / "reader-turned-input.pdf";
        std::ofstream{turned_input, std::ios::binary} << updated;
        decorate(turned_input, letterhead, turned);
        const string drawn = page_text(reader{turned}, 0);
        CHECK(drawn.starts_with("q 0 ")); // a quarter turn: cm's a is 0

        // Passwords on the result, written over its input.
        const auto copy = out / "reader-decorated-in-place.pdf";
        fs::copy_file(report, copy, fs::copy_options::overwrite_existing);
        decorate(copy, letterhead, copy, layer::behind, protection{"open", "admin"});
        note_password(copy, "open");
        CHECK_EQ(reader(copy, "open").page_count(), std::size_t{7});

        // A decoration without pages (made by hand: pdflite writes none), and nothing to merge.
        string none = "%PDF-1.7\n";
        const auto catalog_at = none.size();
        none += "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";
        const auto pages_at = none.size();
        none += "2 0 obj\n<< /Type /Pages /Kids [] /Count 0 >>\nendobj\n";
        const auto xref_at = none.size();
        char entries[64];
        std::snprintf(entries, sizeof entries, "%010zu 00000 n \n%010zu 00000 n \n", catalog_at, pages_at);
        none += string{"xref\n0 3\n0000000000 65535 f \n"} + entries + "trailer\n<< /Size 3 /Root 1 0 R >>\nstartxref\n" +
                std::to_string(xref_at) + "\n%%EOF\n";
        const reader empty = reader::from_memory(none);
        CHECK_EQ(empty.page_count(), std::size_t{0});
        CHECK_THROWS_AS(decorate(reader{report}, empty, out / "reader-never.pdf"), std::invalid_argument);
        CHECK_THROWS_AS(merge(std::vector<fs::path>{}, out / "reader-never.pdf"), std::invalid_argument);
        CHECK(!fs::exists(out / "reader-never.pdf"));
    }

    TEST_SUMMARY();
}
