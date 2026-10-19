## TESTS ##
set(SC_TEST_LINK_LIBRARIES sc-pdflite)

# pdflite.cpp writes its PDFs into pdflite-output, then each checker that's installed (qpdf,
# pdfcpu, mutool) reads every one of them (check_pdfs.cmake); one that isn't is left out, as on a
# server without it.
set(pdflite_output "${CMAKE_CURRENT_BINARY_DIR}/pdflite-output")
add_sc_test(pdflite LABELS unit LINK_LIBRARIES sc-pdflite ZLIB::ZLIB)
set_tests_properties(test-pdflite PROPERTIES
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
