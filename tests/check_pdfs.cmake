# Runs one PDF checker over every PDF in a folder, for ctest (tests/CMakeLists.txt):
#   cmake -DCHECKER=qpdf|pdfcpu|mutool -DTOOL=<its program> -DFOLDER=<folder> -P check_pdfs.cmake
# A file fails when the checker exits with an error or says anything beyond "it's fine": qpdf
# --check's warnings, pdfcpu's strict validation, mutool's warnings while it draws every page.
cmake_minimum_required(VERSION 3.22)

file(GLOB pdfs LIST_DIRECTORIES false "${FOLDER}/*.pdf")
if (NOT pdfs)
    message(FATAL_ERROR "no PDFs in ${FOLDER}: the test that writes them didn't run")
endif ()

set(failures 0)
foreach (pdf IN LISTS pdfs)
    get_filename_component(name "${pdf}" NAME)
    if (CHECKER STREQUAL "qpdf")
        # Exit 0: no errors or warnings (2 is errors, 3 warnings).
        execute_process(COMMAND "${TOOL}" --check "${pdf}" RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
        set(bad FALSE)
        if (NOT status EQUAL 0 OR err MATCHES "." OR out MATCHES "WARNING")
            set(bad TRUE)
        endif ()
    elseif (CHECKER STREQUAL "pdfcpu")
        execute_process(COMMAND "${TOOL}" validate -m strict "${pdf}" RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
        set(bad FALSE)
        if (NOT status EQUAL 0 OR NOT "${out}${err}" MATCHES "validation ok")  # it reports on stderr
            set(bad TRUE)
        endif ()
    elseif (CHECKER STREQUAL "mutool")
        # Draws every page (to nowhere), so content streams and fonts are read too; any message on
        # stderr is a warning or an error.
        execute_process(COMMAND "${TOOL}" draw -q -F png -o "${FOLDER}/.mutool-%d.png" "${pdf}" RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
        file(GLOB drawn "${FOLDER}/.mutool-*.png")
        if (drawn)
            file(REMOVE ${drawn})
        endif ()
        set(bad FALSE)
        if (NOT status EQUAL 0 OR err MATCHES ".")
            set(bad TRUE)
        endif ()
    else ()
        message(FATAL_ERROR "unknown checker ${CHECKER}")
    endif ()
    if (bad)
        math(EXPR failures "${failures} + 1")
        message("FAILED ${CHECKER}: ${name} (exit ${status})\n${out}${err}")
    else ()
        message("ok ${CHECKER}: ${name}")
    endif ()
endforeach ()

if (failures GREATER 0)
    message(FATAL_ERROR "${failures} PDF(s) failed ${CHECKER}")
endif ()
