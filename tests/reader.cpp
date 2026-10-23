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
    const fs::path pictures{SC_PDFLITE_TEST_PICTURES};

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

    SECTION("Not a PDF: std::runtime_error");
    {
        CHECK_THROWS_AS(reader::from_memory("Hello, world"), std::runtime_error);
        CHECK_THROWS_AS(reader::from_memory("%PDF-1.7\nnothing more\n"), std::runtime_error); // no objects at all
        CHECK_THROWS_AS(reader(out / "no-such-file.pdf"), std::runtime_error);
        const reader pdf{fixtures / "quartz.pdf"};
        CHECK(!pdf.repaired());
        CHECK_THROWS_AS(pdf.decoded(sc::pdflite::stream{dictionary{{"Filter", name{"DCTDecode"}}}, "x"}), std::runtime_error);
    }

    SECTION("Damaged files repaired: the cross-reference rebuilt by scanning, pages found without their tree");
    {
        // Each damaged copy is read as the sound one is: same pages, same content.
        auto same_as = [&](const reader &damaged, const fs::path &sound, const std::string &password = {}) {
            const reader original{sound, password};
            if (damaged.page_count() != original.page_count()) return false;
            for (std::size_t i = 0; i < original.page_count(); ++i)
                if (page_text(damaged, i) != page_text(original, i)) return false;
            return true;
        };
        // Bytes inserted after the header: every offset in the cross-reference is off.
        auto shifted = [](const string &pdf) {
            const auto line = pdf.find('\n') + 1;
            return pdf.substr(0, line) + "% a comment that moves every object along\n" + pdf.substr(line);
        };
        const auto quartz = read(fixtures / "quartz.pdf");

        // Cut short before its cross-reference table: no xref, trailer or startxref.
        const reader cut = reader::from_memory(quartz.substr(0, quartz.rfind("\nxref") + 1));
        CHECK(cut.repaired());
        CHECK(same_as(cut, fixtures / "quartz.pdf")); // the catalog found by its /Type
        // startxref pointing nowhere.
        string wrong = quartz;
        wrong.replace(wrong.rfind("startxref") + 10, wrong.size() - wrong.rfind("startxref") - 10, "99999999\n%%EOF\n");
        const reader nowhere = reader::from_memory(wrong);
        CHECK(nowhere.repaired());
        CHECK(same_as(nowhere, fixtures / "quartz.pdf"));
        // Every offset off.
        const reader moved = reader::from_memory(shifted(quartz));
        CHECK(moved.repaired());
        CHECK(same_as(moved, fixtures / "quartz.pdf"));

        // A file whose objects are in object streams, its cross-reference stream lost.
        const auto objstm = read(fixtures / "objstm.pdf");
        string lost = objstm;
        lost.replace(lost.rfind("startxref") + 10, lost.size() - lost.rfind("startxref") - 10, "12\n%%EOF\n");
        const reader streams = reader::from_memory(lost);
        CHECK(streams.repaired());
        CHECK(same_as(streams, fixtures / "objstm.pdf"));
        CHECK_EQ(title(streams), string{"Check"});

        // Encrypted, and moved: decrypted once repaired (AES-256 with object streams, and RC4).
        const reader aes = reader::from_memory(shifted(read(fixtures / "aes-256-objstm.pdf")), "user");
        CHECK(aes.repaired());
        CHECK(same_as(aes, fixtures / "aes-256-objstm.pdf", "user"));
        CHECK_EQ(title(aes), string{"Check"});
        const reader rc4 = reader::from_memory(shifted(read(fixtures / "rc4-40.pdf")));
        CHECK(rc4.repaired());
        CHECK(same_as(rc4, fixtures / "rc4-40.pdf"));

        // Cut short with its trailer: RC4 and AES-128 need the file's /ID for their key, so it says
        // so rather than read encrypted bytes as content. AES-256 doesn't: it's read.
        const auto rc4_whole = read(fixtures / "rc4-40.pdf");
        try {
            const reader cut_rc4 = reader::from_memory(rc4_whole.substr(0, rc4_whole.rfind("\nxref") + 1));
            CHECK(cut_rc4.page_count() == 0); // not reached
            CHECK(false);
        } catch (const std::runtime_error &error) {
            CHECK(string{error.what()}.find("identifier") != string::npos);
        }

        // A stream's /Length wrong: its data found by its endstream, nothing rebuilt.
        auto own_file = read(own(out / "reader-length.pdf", "Epsilon"));
        const auto length_at = own_file.find("/Length ") + 8;
        const auto digits = own_file.find_first_of(" />\n", length_at) - length_at;
        own_file.replace(length_at, digits, string(digits - 1, '0') + "1"); // as many digits: nothing else moves
        const reader length = reader::from_memory(own_file);
        CHECK(!length.repaired());
        CHECK(page_text(length, 0).find("(Epsilon 1) Tj") != string::npos ||
              page_text(length, 1).find("(Epsilon 2) Tj") != string::npos); // whichever stream it was, all still read

        // The page tree broken (/Kids misspelt): the pages taken in the order they're in the file.
        auto broken_tree = read(own(out / "reader-tree.pdf", "Zeta"));
        broken_tree.replace(broken_tree.find("/Kids"), 5, "/Kidz");
        const reader loose = reader::from_memory(broken_tree);
        CHECK(loose.repaired());
        CHECK_EQ(loose.page_count(), std::size_t{3});
        if (loose.page_count() == 3) {
            CHECK(page_text(loose, 0).find("(Zeta 1) Tj") != string::npos);
            CHECK(page_text(loose, 2).find("(Zeta 3) Tj") != string::npos);
            CHECK(loose.page(1).contains("MediaBox"));
        }

        // A damaged file merged with a sound one: written whole (ctest's checkers read it too).
        const auto damaged_file = out / "reader-damaged-input.pdf";
        std::ofstream{damaged_file, std::ios::binary} << shifted(quartz);
        const auto merged = out / "reader-merged-damaged.pdf";
        merge({damaged_file, fixtures / "mutool.pdf"}, merged);
        const reader result{merged};
        CHECK(!result.repaired());
        CHECK_EQ(result.page_count(), std::size_t{2});
        fs::remove(damaged_file); // it's damaged: not one for the checkers

        // Half a file: what can be found is read, or nothing is: it doesn't crash.
        try {
            const reader half = reader::from_memory(quartz.substr(0, quartz.size() / 2));
            CHECK(half.repaired());
            CHECK(half.page_count() <= 1);
        } catch (const std::runtime_error &) {
            CHECK(true);
        }
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

    SECTION("merge() with pictures: a JPEG or PNG a page the size of the page before it, upright, fitted, centred");
    {
        auto media_box = [](const reader &pdf, const std::size_t page) {
            const auto box = pdf.resolve(pdf.page(page).at("MediaBox")).as<array>();
            auto n = [&](const std::size_t i) { return box[i].is<double>() ? box[i].as<double>() : static_cast<double>(box[i].as<std::int64_t>()); };
            return std::pair{n(2) - n(0), n(3) - n(1)};
        };
        // After an A5 file: A5 pages. (own() writes A5: 419.53 x 595.28 points.)
        const auto report = own(out / "reader-pictures-report.pdf", "Theta");
        const auto merged = out / "reader-pictures.pdf";
        merge({report, pictures / "exif-1.jpg", pictures / "exif-6.jpg", pictures / "tall.png"}, merged);
        const reader pdf{merged};
        CHECK_EQ(pdf.page_count(), std::size_t{6});
        const auto a5 = media_box(pdf, 0);
        bool same_size = true;
        for (std::size_t page = 3; page < 6; ++page) same_size = same_size && media_box(pdf, page) == a5;
        CHECK(same_size);
        // Upright: orientation 1 is drawn as it is; 6 turned a quarter (cm's a is 0).
        const string upright_page = page_text(pdf, 3), turned_page = page_text(pdf, 4);
        CHECK(upright_page.starts_with("q\n419.5276 0 0 279.6851 0 157.7953 cm")); // the page's width, centred: 60 x 40 to 419.5 x 279.7
        CHECK(turned_page.starts_with("q\n0 "));                                   // a quarter turn: cm's a is 0
        CHECK(turned_page.find(" Do") != string::npos);
        CHECK_EQ(title(pdf), string{"Theta"}); // still the report's information

        // A picture first: the next file's size; no PDF at all: A4.
        const auto first = out / "reader-picture-first.pdf";
        merge({pictures / "tall.png", fixtures / "quartz.pdf"}, first);
        const reader starts{first};
        const auto quartz = media_box(reader{fixtures / "quartz.pdf"}, 0);
        CHECK((media_box(starts, 0) == quartz));
        const auto only = out / "reader-pictures-only.pdf";
        merge({pictures / "exif-1.jpg", pictures / "tall.png"}, only);
        const auto a4 = media_box(reader{only}, 0);
        CHECK(std::abs(a4.first - 595.2756) < 0.01 && std::abs(a4.second - 841.8898) < 0.01);

        // Neither a PDF nor a picture sc reads: the error names the file.
        const auto text = out / "reader-not-a-picture.txt";
        std::ofstream{text} << "just text";
        try {
            merge({report, text}, out / "reader-never.pdf");
            CHECK(false);
        } catch (const std::runtime_error &error) {
            CHECK(string{error.what()}.find("reader-not-a-picture.txt") != string::npos);
        }
        CHECK(!fs::exists(out / "reader-never.pdf"));
        fs::remove(text);
        // Nothing left behind in the temporary folder.
        bool leftovers = false;
        for (const auto &entry: fs::directory_iterator{fs::temp_directory_path()})
            leftovers = leftovers || entry.path().filename().string().starts_with("sc-pdflite-picture-");
        CHECK(!leftovers);
    }

    TEST_SUMMARY();
}
