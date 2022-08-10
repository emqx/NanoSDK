#
# This software is supplied under the terms of the MIT License, a
# copy of which should be located in the distribution where this
# file was obtained (LICENSE.txt).  A copy of the license may also be
# found online at https://opensource.org/licenses/MIT.
#
if(NNG_QUIC_LIB STREQUAL "msquic")
        message(WARNING "
        ************************************************************
                        Taking MSQUIC as quic lib
        ************************************************************")
        add_subdirectory(${PROJECT_SOURCE_DIR}/extern/msquic msquic)
#     target_include_directories(msquic PRIVATE)
#     nng_check_lib(msquic)

    nng_defines(QUIC_TLS_ENGINE_INIT=nng_quic_libs_init_msquic)
    nng_defines(QUIC_TLS_ENGINE_FINI=nng_quic_libs_fini_msquic)
    nng_defines(NNG_SUPP_QUIC)
endif ()
