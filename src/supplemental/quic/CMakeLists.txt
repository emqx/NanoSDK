#
# This software is supplied under the terms of the MIT License, a
# copy of which should be located in the distribution where this
# file was obtained (LICENSE.txt).  A copy of the license may also be
# found online at https://opensource.org/licenses/MIT.
#

if (NNG_ENABLE_QUIC)
    set(NNG_QUIC_LIBS msquic none)

    # We assume msquic for now.
    set(NNG_QUIC_LIB msquic CACHE STRING "QUIC lib to use.")
    add_subdirectory(msquic)
    add_dependencies(nng msquic)

    nng_sources(quic_api.c)
    nng_sources(quic_api.h)

    find_path(INTERNAL_MSQUIC_INCLUDE_DIR
    NAMES msquic.h
    HINTS ${_MSQUIC_ROOT_HINTS}
    PATHS ${PROJECT_SOURCE_DIR}/extern/msquic/src
    PATH_SUFFIXES inc)

    if(NNG_PLATFORM_POSIX)
        nng_link_libraries(dl)
    elseif(NNG_PLATFORM_WINDOWS)
        nng_link_libraries(ws2_32)
    endif()

    nng_link_libraries("${CMAKE_SOURCE_DIR}/build/_deps/opensslquic-build/openssl/lib/libssl.a")
    nng_link_libraries("${CMAKE_SOURCE_DIR}/build/_deps/opensslquic-build/openssl/lib/libcrypto.a")
    nng_include_directories(${CMAKE_SOURCE_DIR}/build/_deps/opensslquic-build/openssl/include)

    nng_include_directories(${INTERNAL_MSQUIC_INCLUDE_DIR})
    
    # set_property(CACHE NNG_QUIC_LIB PROPERTY STRINGS ${NNG_TLS_ENGINES})
else ()
    set(NNG_QUIC_LIB none)
endif ()

# if (NOT NNG_QUIC_LIB STREQUAL "none")
#     nng_test(quic_test)
# endif ()
