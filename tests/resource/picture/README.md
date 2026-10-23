# Test pictures for merge()

Made 2026-10-23 with Pillow: one 60x40 picture (a red bar and a blue bar, an "F" without its
middle) stored upright with EXIF orientation 1 (`exif-1.jpg`), and stored turned a quarter
anticlockwise with orientation 6 (`exif-6.jpg`), which shows it upright again; and a 30x90 PNG
with an alpha channel (`tall.png`). `tests/reader.cpp` appends them to PDFs.
