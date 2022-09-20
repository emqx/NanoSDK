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
    set(_MSQUIC_ROOT_HINTS ${MSQUIC_ROOT_DIR} ENV MSQUIC_ROOT_DIR)
    add_subdirectory(${PROJECT_SOURCE_DIR}/extern/msquic msquic)

    nng_defines(QUIC_TLS_ENGINE_INIT=nng_quic_libs_init_msquic)
    nng_defines(QUIC_TLS_ENGINE_FINI=nng_quic_libs_fini_msquic)
    nng_defines(NNG_SUPP_QUIC)

    # find_library(MSQUIC_LIBRARIES
    # NAMES msquic
    # PATHS /usr/local/include
    # PATH_SUFFIXES lib)

    find_path(MSQUIC_INCLUDE_DIR
    NAMES msquic.h
    HINTS ${_MSQUIC_ROOT_HINTS}
    PATHS /usr/local
    PATH_SUFFIXES include)

    find_library(MSQUIC_LIBRARIES
        NAMES msquic
        HINTS ${_MSQUIC_ROOT_HINTS}
        PATHS /usr/local
        PATH_SUFFIXES lib)

    set(QUIC_LIBRARIES
        ${MSQUIC_LIBRARIES})
    mark_as_advanced(QUIC_LIBRARIES)

    message("*************quic library ${QUIC_LIBRARIES}:${MSQUIC_LIBRARIES}************")

    if(NNG_PROTO_MQTT_BROKER)
        message(" ** Build msquic with nanonng ** ")
        # nng_link_libraries(${QUIC_LIBRARIES})
        include_directories(${PROJECT_SOURCE_DIR}/extern/msquic)
    else()
        message(" ** Build msquic with nanosdk ** ")
        nng_link_libraries(${QUIC_LIBRARIES})
        nng_include_directories(${MSQUIC_INCLUDE_DIR})
    endif()
endif()
