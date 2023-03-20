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

    # In sdk. use openssl from system
    find_package(OpenSSL REQUIRED)
    if(OPENSSL_FOUND)
        include_directories(${OPENSSL_INCLUDE_DIRS})
        nng_link_libraries(OpenSSL::SSL OpenSSL::Crypto)
    endif()

    #nng_include_directories(${CMAKE_SOURCE_DIR}/build/_deps/opensslquic-build/openssl/include)
    #nng_link_libraries("${CMAKE_SOURCE_DIR}/build/_deps/opensslquic-build/openssl/lib/libssl.a")
    #nng_link_libraries("${CMAKE_SOURCE_DIR}/build/_deps/opensslquic-build/openssl/lib/libcrypto.a")

    # set_property(CACHE NNG_QUIC_LIB PROPERTY STRINGS ${NNG_TLS_ENGINES})
else ()
    set(NNG_QUIC_LIB none)
endif ()

# if (NOT NNG_QUIC_LIB STREQUAL "none")
#     nng_test(quic_test)
# endif ()
