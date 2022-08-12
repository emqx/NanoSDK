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
    include_directories(${PROJECT_SOURCE_DIR}/extern/msquic)

    nng_sources(quic_api.c)
    nng_sources(quic_api.h)
    set_property(CACHE NNG_QUIC_LIB PROPERTY STRINGS ${NNG_TLS_ENGINES})
else ()
    set(NNG_QUIC_LIB none)
endif ()

# default QUIC implementation for now is msquic.
# nng_headers(nng/supplemental/quic/quic_api.h)
# nng_headers(nng/supplemental/quic/engine.h)

# if (NOT NNG_QUIC_LIB STREQUAL "none")
#     nng_test(quic_test)
# endif ()
