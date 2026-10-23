# Test PDFs for the reader

Made 2026-10-23 from one 7-page sc-html2pdf report (`Check`, by `Test User`), so the reader is
tested on files it didn't write itself. Each is read, merged and protected by `tests/reader.cpp`.

| File | Made with | What it tests |
|---|---|---|
| `objstm.pdf` | `qpdf --object-streams=generate` | a cross-reference stream (PNG predictor) and object streams |
| `linearized.pdf` | `qpdf --linearize` | a linearized file: two cross-reference sections |
| `rc4-40.pdf` | `qpdf --allow-weak-crypto --encrypt "" owner 40` | RC4, 40 bits (revision 2); no password needed to open |
| `rc4-128.pdf` | `qpdf --allow-weak-crypto --encrypt user owner 128 --use-aes=n` | RC4, 128 bits (revision 3); user `user`, owner `owner` |
| `aes-128-owner.pdf` | `qpdf --encrypt "" owner 128 --use-aes=y` | AES-128 (revision 4, crypt filters); an owner password only |
| `aes-256-objstm.pdf` | `qpdf --encrypt user owner 256 -- --object-streams=generate` | AES-256 (revision 6) with object streams |
| `quartz.pdf` | `cupsfilter -m application/pdf` on a text file | macOS's Quartz PDFContext, one page |
| `mutool.pdf` | `mutool create` | MuPDF's writer, a built-in font, one page |
