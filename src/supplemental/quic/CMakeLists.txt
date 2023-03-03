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

    find_library(OPENSSLQUIC_LIBRARIES
    NAMES openssl ssl libssl libopenssl
    HINTS "${CMAKE_SOURCE_DIR}/build/_deps/opensslquic-build/openssl"
    PATHS "${CMAKE_SOURCE_DIR}/build/_deps/opensslquic-build/openssl"
    PATH_SUFFIXES lib)

find_library(CRYPTOQUIC_LIBRARIES
    NAMES crypto libcrypto
    HINTS "${CMAKE_SOURCE_DIR}/build/_deps/opensslquic-build/openssl"
    PATHS "${CMAKE_SOURCE_DIR}/build/_deps/opensslquic-build/openssl"
    PATH_SUFFIXES lib)

 if(OPENSSLQUIC_LIBRARIES_NOTFOUND)
     message("No openssl for MsQuic be found!")
 elif(CRYPTOQUIC_LIBRARIES_NOTFOUND)
     message("No openssl for MsQuic be found!")
 else()
     nng_include_directories(${CMAKE_SOURCE_DIR}/build/_deps/opensslquic-build/openssl/include)

     set(SSLQUIC_LIBRARIES ${OPENSSLQUIC_LIBRARIES})
     mark_as_advanced(SSLQUIC_LIBRARIES)
     nng_link_libraries(${SSLQUIC_LIBRARIES})

     set(SSLCRYPTO_LIBRARIES ${CRYPTOQUIC_LIBRARIES})
     mark_as_advanced(CRYPTOQUIC_LIBRARIES)
     nng_link_libraries(${CRYPTOQUIC_LIBRARIES})
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
