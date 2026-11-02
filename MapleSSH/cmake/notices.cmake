set(notice_files
    "${MAPLE_WX_SOURCE}/docs/licence.txt"
    "${MAPLE_WX_SOURCE}/docs/lgpl.txt"
    "${MAPLE_WX_SOURCE}/docs/gpl.txt"
    "${MAPLE_WX_SOURCE}/src/expat/expat/COPYING"
    "${MAPLE_WX_SOURCE}/src/png/LICENSE"
    "${MAPLE_WX_SOURCE}/src/zlib/LICENSE"
    "${MAPLE_WX_SOURCE}/3rdparty/pcre/LICENCE"
    "${MAPLE_WX_SOURCE}/3rdparty/nanosvg/LICENSE.txt"
    "${MAPLE_DOWNLOADS}/libvterm-0.3.3/LICENSE"
    "${MAPLE_DOWNLOADS}/libssh2-1.11.1/COPYING"
    "${MAPLE_DOWNLOADS}/openssl-3.5.9/LICENSE.txt"
    "${MAPLE_DOWNLOADS}/json-LICENSE.MIT")
file(WRITE "${CMAKE_BINARY_DIR}/third-party.txt" "MapleSSH third-party licenses and notices\n\n")
foreach(notice IN LISTS notice_files)
    if(NOT EXISTS "${notice}")
        message(FATAL_ERROR "Missing required third-party notice: ${notice}")
    endif()
    file(READ "${notice}" text)
    get_filename_component(parent "${notice}" DIRECTORY)
    get_filename_component(component "${parent}" NAME)
    get_filename_component(filename "${notice}" NAME)
    file(APPEND "${CMAKE_BINARY_DIR}/third-party.txt" "\n======== ${component} / ${filename} ========\n${text}\n")
endforeach()
