#
# This software is supplied under the terms of the MIT License, a
# copy of which should be located in the distribution where this
# file was obtained (LICENSE.txt).  A copy of the license may also be
# found online at https://opensource.org/licenses/MIT.
#

if (NNG_ENABLE_QUIC)
    nng_sources(quic_api.c)
    nng_sources(quic_api.h)
    find_package(msquic)
    nng_link_libraries(msquic)

endif ()

# default QUIC implementation for now is msquic.
# nng_headers(nng/supplemental/quic/quic_api.h)
# nng_headers(nng/supplemental/quic/engine.h)

# if (NOT NNG_QUIC_LIB STREQUAL "none")
#     nng_test(quic_test)
# endif ()
