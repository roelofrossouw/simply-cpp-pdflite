# Runs one PDF checker over every PDF in a folder, for ctest (tests/CMakeLists.txt):
#   cmake -DCHECKER=qpdf|pdfcpu|mutool -DTOOL=<its program> -DFOLDER=<folder> -P check_pdfs.cmake
# A file fails when the checker exits with an error or says anything beyond "it's fine": qpdf
# --check's warnings, pdfcpu's strict validation, mutool's warnings while it draws every page.
cmake_minimum_required(VERSION 3.22)

file(GLOB pdfs LIST_DIRECTORIES false "${FOLDER}/*.pdf")
if (NOT pdfs)
    message(FATAL_ERROR "no PDFs in ${FOLDER}: the test that writes them didn't run")
endif ()

# Ubuntu 26.04 confines qpdf with AppArmor (/etc/apparmor.d/qpdf) to PDFs under home folders, /tmp,
# /mnt, /media, /usr and /opt, so the build servers' /var/www/build is out of its reach: it reads
# copies in a folder of its own under /tmp.
string(RANDOM LENGTH 12 suffix)
set(copies "/tmp/sc-pdflite-check-${suffix}")
if (CHECKER STREQUAL "qpdf")
    file(MAKE_DIRECTORY "${copies}")
endif ()

set(failures 0)
foreach (pdf IN LISTS pdfs)
    get_filename_component(name "${pdf}" NAME)
    # An encrypted file's password, in <file>.password beside it, given to each checker.
    set(password "")
    if (EXISTS "${pdf}.password")
        file(READ "${pdf}.password" password)
    endif ()
    set(qpdf_password "")
    set(pdfcpu_password "")
    set(mutool_password "")
    if (NOT password STREQUAL "")
        set(qpdf_password "--password=${password}")
        set(pdfcpu_password --upw "${password}")
        set(mutool_password -p "${password}")
    endif ()
    if (CHECKER STREQUAL "qpdf")
        # Exit 0: no errors or warnings (2 is errors, 3 warnings).
        file(COPY_FILE "${pdf}" "${copies}/${name}")
        execute_process(COMMAND "${TOOL}" ${qpdf_password} --check "${copies}/${name}" RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
        file(REMOVE "${copies}/${name}")
        set(bad FALSE)
        if (NOT status EQUAL 0 OR err MATCHES "." OR out MATCHES "WARNING")
            set(bad TRUE)
        endif ()
    elseif (CHECKER STREQUAL "pdfcpu")
        execute_process(COMMAND "${TOOL}" validate -m strict ${pdfcpu_password} "${pdf}" RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
        set(bad FALSE)
        if (NOT status EQUAL 0 OR NOT "${out}${err}" MATCHES "validation ok")  # it reports on stderr
            set(bad TRUE)
        endif ()
    elseif (CHECKER STREQUAL "mutool")
        # Draws every page (to nowhere), so content streams and fonts are read too; any message on
        # stderr is a warning or an error.
        execute_process(COMMAND "${TOOL}" draw -q ${mutool_password} -F png -o "${FOLDER}/.mutool-%d.png" "${pdf}" RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err)
        file(GLOB drawn "${FOLDER}/.mutool-*.png")
        if (drawn)
            file(REMOVE ${drawn})
        endif ()
        # Ubuntu's MuPDF is built without colour management and says so about any file; that
        # isn't about the PDF, so it's the one message let through.
        string(REPLACE "warning: ICC support is not available\n" "" err "${err}")
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

file(REMOVE_RECURSE "${copies}")

if (failures GREATER 0)
    message(FATAL_ERROR "${failures} PDF(s) failed ${CHECKER}")
endif ()
