## TESTS ##
set(SC_TEST_LINK_LIBRARIES sc-pdflite)

# pdflite.cpp writes its PDFs into pdflite-output, then each checker that's installed (qpdf,
# pdfcpu, mutool) reads every one of them (check_pdfs.cmake); one that isn't is left out, as on a
# server without it.
set(pdflite_output "${CMAKE_CURRENT_BINARY_DIR}/pdflite-output")
add_sc_test(pdflite LABELS unit LINK_LIBRARIES sc-pdflite ZLIB::ZLIB PkgConfig::freetype2)
target_compile_definitions(test-pdflite PRIVATE SC_PDFLITE_TEST_PHOTO="${CMAKE_CURRENT_SOURCE_DIR}/../examples/resource/cat.jpg"
        SC_PDFLITE_TEST_PNGS="${CMAKE_CURRENT_SOURCE_DIR}/resource/png"
        # Fonts beyond what fontconfig finds, when they're there: a variable font (litehtml-tests' Noto
        # Sans, from sc-litehtml's build) and a CFF one (macOS's SF Pro); their tests are skipped otherwise.
        SC_PDFLITE_TEST_VARIABLE_FONT="${CMAKE_CURRENT_SOURCE_DIR}/../../sc-litehtml/cmake-build-local/sc-litehtml-test/litehtml-tests-f750c7c4c1f0b2db63049f45ce505442b2685fa6/fonts/NotoSans-VariableFont_wdth,wght.ttf"
        SC_PDFLITE_TEST_CFF_FONT="/Library/Fonts/SF-Pro-Display-SemiboldItalic.otf")
set_tests_properties(test-pdflite PROPERTIES
        ENVIRONMENT "SC_PDFLITE_OUTPUT=${pdflite_output}"
        FIXTURES_SETUP pdflite_files)
# The reader, merge() and protect(), on pdflite's own files and on others' (resource/pdf).
add_sc_test(reader LABELS unit)
target_compile_definitions(test-reader PRIVATE SC_PDFLITE_TEST_PDFS="${CMAKE_CURRENT_SOURCE_DIR}/resource/pdf")
set_tests_properties(test-reader PROPERTIES
        ENVIRONMENT "SC_PDFLITE_OUTPUT=${pdflite_output}"
        FIXTURES_SETUP pdflite_files)
foreach (checker IN ITEMS qpdf pdfcpu mutool)
    find_program(${checker}_program ${checker})
    if (${checker}_program)
        add_test(NAME check-${checker}
                COMMAND ${CMAKE_COMMAND} -DCHECKER=${checker} -DTOOL=${${checker}_program} -DFOLDER=${pdflite_output}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/check_pdfs.cmake)
        set_tests_properties(check-${checker} PROPERTIES LABELS compliance FIXTURES_REQUIRED pdflite_files)
    else ()
        message(STATUS "${checker} not found: the PDFs aren't checked with it")
    endif ()
endforeach ()
