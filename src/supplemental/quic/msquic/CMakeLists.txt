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

    if(BUILD_STATIC)
        set(QUIC_BUILD_SHARED OFF)
    endif()
    
    add_subdirectory(${PROJECT_SOURCE_DIR}/extern/msquic msquic)

    nng_defines(QUIC_TLS_ENGINE_INIT=nng_quic_libs_init_msquic)
    nng_defines(QUIC_TLS_ENGINE_FINI=nng_quic_libs_fini_msquic)
    nng_defines(NNG_SUPP_QUIC)

    # Prefer the in-tree msquic target we just added. Looking up a physical
    # library file here fails on first configure because the artifact is not
    # built yet.
    if(TARGET msquic)
        set(MSQUIC_LIBRARIES msquic)
        set(MSQUIC_INCLUDE_DIR ${PROJECT_SOURCE_DIR}/extern/msquic/src/inc)
    else()
        find_path(MSQUIC_INCLUDE_DIR
            NAMES msquic.h
            HINTS ${_MSQUIC_ROOT_HINTS}
            PATHS /usr/local
            PATH_SUFFIXES include)

        if(BUILD_STATIC)
            set(CMAKE_FIND_LIBRARY_SUFFIXES ${CMAKE_STATIC_LIBRARY_SUFFIX})
        endif()

        find_library(MSQUIC_LIBRARIES
            NAMES msquic
            HINTS ${_MSQUIC_ROOT_HINTS}
            PATHS /usr/local
            PATH_SUFFIXES lib)

        unset(CMAKE_FIND_LIBRARY_SUFFIXES)
    endif()

    set(QUIC_LIBRARIES
        ${MSQUIC_LIBRARIES})
    mark_as_advanced(QUIC_LIBRARIES)

    if(NNG_PROTO_MQTT_BROKER)
        message(" ** Build msquic with nanonng ** ")
    else()
        message(" ** Build msquic with nanosdk ** ")
        # msquic_dial.c uses OpenSSL X509 APIs directly; export this dependency
        # so downstream static links (e.g. demos using installed nng) resolve.
        nng_find_package(OpenSSL)
        nng_link_libraries(${QUIC_LIBRARIES} OpenSSL::Crypto)
        nng_include_directories(${MSQUIC_INCLUDE_DIR})
        message(" ******* quic library ${QUIC_LIBRARIES}:${MSQUIC_LIBRARIES} ******* ")
    endif()
endif()
