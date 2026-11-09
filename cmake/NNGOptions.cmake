#
# Copyright 2024 Staysail Systems, Inc. <info@staysail.tech>
#
# This software is supplied under the terms of the MIT License, a
# copy of which should be located in the distribution where this
# file was obtained (LICENSE.txt).  A copy of the license may also be
# found online at https://opensource.org/licenses/MIT.
#

# NNG Options.  These are user configurable knobs.

include(CMakeDependentOption)

if (CMAKE_CROSSCOMPILING OR BUILD_STATIC)
    set(NNG_NATIVE_BUILD OFF)
else ()
    set(NNG_NATIVE_BUILD ON)
endif ()

# Global options.
option(BUILD_SHARED_LIBS "Build shared library" ${BUILD_SHARED_LIBS})

# We only build command line tools and tests if we are not in a
# cross-compile situation.  Cross-compiling users who still want to
# build these must enable them explicitly.  Some of these switches
# must be enabled rather early as we use their values later.
option(NNG_TESTS "Build and run tests." ${NNG_NATIVE_BUILD})
option(NNG_TOOLS "Build extra tools." ${NNG_NATIVE_BUILD})
option(NNG_ENABLE_NNGCAT "Enable building nngcat utility." ${NNG_TOOLS})
option(NNG_ENABLE_COVERAGE "Enable coverage reporting." OFF)

message(NNG_TESTS = "${NNG_TESTS}")
message(NNG_TOOLS = "${NNG_TOOLS}")

# Eliding deprecated functionality can be used to build a slimmed down
# version of the library, or alternatively to test for application
# preparedness for expected feature removals (in the next major release.)
# Applications can also set the NNG_ELIDE_DEPRECATED preprocessor symbol
# before including <nng/nng.h> -- this will prevent declarations from
# being exposed to applications, but it will not affect their ABI
# availability for existing compiled applications.
# Note: Currently this breaks the test suite, so we only do it
# for the public library.
option(NNG_ELIDE_DEPRECATED "Elide deprecated functionality." OFF)

# Turning off the compatibility layer can save some space, and
# compilation time, but may break legacy applications  It should
# be left enabled when building a shared library.
option(NNG_ENABLE_COMPAT "Enable legacy nanomsg API." ON)

option(NNG_ENABLE_STATS "Enable statistics." ON)
mark_as_advanced(NNG_ENABLE_STATS)

# SQLITE API support.
option (NNG_ENABLE_SQLITE "Enable SQLITE API." OFF)
if (NNG_ENABLE_SQLITE)
    set(NNG_SUPP_SQLITE ON)
endif()
mark_as_advanced(NNG_ENABLE_SQLITE)

# Preserve existing Linux/Unix payload compression. Windows/macOS default
# builds do not require a zstd SDK; users can opt in when it is installed.
set(NNG_ZSTD_DEFAULT ON)
if (WIN32 OR APPLE)
    set(NNG_ZSTD_DEFAULT OFF)
endif ()
option(NNG_ENABLE_ZSTD "Enable Zstandard compression for MQTT payloads." ${NNG_ZSTD_DEFAULT})
unset(NNG_ZSTD_DEFAULT)

# Protocols.
option (NNG_PROTO_BUS0 "Enable BUSv0 protocol." ON)
mark_as_advanced(NNG_PROTO_BUS0)

option (NNG_PROTO_PAIR0 "Enable PAIRv0 protocol." ON)
mark_as_advanced(NNG_PROTO_PAIR0)

option (NNG_PROTO_PAIR1 "Enable PAIRv1 protocol." ON)
mark_as_advanced(NNG_PROTO_PAIR1)

option (NNG_PROTO_PUSH0 "Enable PUSHv0 protocol." ON)
mark_as_advanced(NNG_PROTO_PUSH0)

option (NNG_PROTO_PULL0 "Enable PULLv0 protocol." ON)
mark_as_advanced(NNG_PROTO_PULL0)

option (NNG_PROTO_PUB0 "Enable PUBv0 protocol." ON)
mark_as_advanced(NNG_PROTO_PUB0)

option (NNG_PROTO_SUB0 "Enable SUBv0 protocol." ON)
mark_as_advanced(NNG_PROTO_SUB0)

option(NNG_PROTO_REQ0 "Enable REQv0 protocol." ON)
mark_as_advanced(NNG_PROTO_REQ0)

option(NNG_PROTO_REP0 "Enable REPv0 protocol." ON)
mark_as_advanced(NNG_PROTO_REP0)

option (NNG_PROTO_RESPONDENT0 "Enable RESPONDENTv0 protocol." ON)
mark_as_advanced(NNG_PROTO_RESPONDENT0)

option (NNG_PROTO_SURVEYOR0 "Enable SURVEYORv0 protocol." ON)
mark_as_advanced(NNG_PROTO_SURVEYOR0)

option (NNG_PROTO_MQTT_CLIENT "Enable MQTT Client protocol." ON)
mark_as_advanced(NNG_PROTO_MQTT_CLIENT)

# The SDK QUIC integration is disabled on Windows/macOS, including explicit
# requests and stale cache entries. Other Unix platforms keep their opt-in.
if ((WIN32 OR APPLE) AND NNG_ENABLE_QUIC)
    message(STATUS "MQTT over QUIC is disabled on Windows/macOS")
endif ()
cmake_dependent_option(NNG_ENABLE_QUIC "Enable Quic support." OFF
    "NOT WIN32;NOT APPLE" OFF)

# These layers require the same backend; do not let stale independent options
# enable a protocol or transport after the backend has been disabled.
set(NNG_PROTO_MQTT_QUIC_CLIENT ${NNG_ENABLE_QUIC})
set(NNG_SUPP_QUIC ${NNG_ENABLE_QUIC})

if (NNG_ENABLE_QUIC)
    set(NNG_QUIC_LIBS msquic none)
    # We assume MSQUIC only for now.  (Someday replaced perhaps with ngtcp.)
    set(NNG_QUIC_LIB msquic CACHE STRING "Quic lib to use.")
    set_property(CACHE NNG_QUIC_LIB PROPERTY STRINGS ${NNG_QUIC_LIBS})
else ()
    set(NNG_QUIC_LIB none)
endif ()

# The current SCRAM implementation uses POSIX strings and C99 VLAs.
cmake_dependent_option(NNG_ENABLE_SCRAM "Enable SCRAM support." OFF
    "NOT WIN32" OFF)
if (NNG_ENABLE_SCRAM)
    set(SUPP_SCRAM ON)
endif ()

# TLS support.

# Enabling TLS is required to enable support for the TLS transport
# and WSS.  It does require a 3rd party TLS engine to be selected.
option(NNG_ENABLE_TLS "Enable TLS support." OFF)
if (NNG_ENABLE_TLS)
    set(NNG_SUPP_TLS ON)
endif ()

if (NNG_ENABLE_TLS)
    set(NNG_TLS_ENGINES mbed wolf open none)
    # We assume Mbed for now.  (Someday replaced perhaps with Bear.)
    set(NNG_TLS_ENGINE mbed CACHE STRING "TLS engine to use.")
    set_property(CACHE NNG_TLS_ENGINE PROPERTY STRINGS ${NNG_TLS_ENGINES})
else ()
    set(NNG_TLS_ENGINE none)
endif ()

# Optional OpenSSL vendor extensions. Ordinary OpenSSL stays the default.
option(NNG_TLS_OPENSSL_HAVE_GM "Enable TASSL GM/TLS support." OFF)
# The bundled Nanwang engine and vendor libraries are Linux-only.
set(NNG_NANWANG_SUPPORTED OFF)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(NNG_NANWANG_SUPPORTED ON)
endif ()
cmake_dependent_option(NNG_TLS_OPENSSL_HAVE_GM_NW
    "Enable Nanwang GM/TLS support (Linux only)." OFF
    "NNG_NANWANG_SUPPORTED" OFF)
unset(NNG_NANWANG_SUPPORTED)

# HTTP API support.
option (NNG_ENABLE_HTTP "Enable HTTP API." ON)
if (NNG_ENABLE_HTTP)
    set(NNG_SUPP_HTTP ON)
endif()
mark_as_advanced(NNG_ENABLE_HTTP)

# Some sites or kernels lack IPv6 support.  This override allows us
# to prevent the use of IPv6 in environments where it isn't supported.
option (NNG_ENABLE_IPV6 "Enable IPv6." ON)
mark_as_advanced(NNG_ENABLE_IPV6)

#
# Transport Options.
#

option (NNG_TRANSPORT_INPROC "Enable inproc transport." ON)
mark_as_advanced(NNG_TRANSPORT_INPROC)

option (NNG_TRANSPORT_IPC "Enable IPC transport." ON)
mark_as_advanced(NNG_TRANSPORT_IPC)

# TCP transport
option (NNG_TRANSPORT_TCP "Enable TCP transport." ON)
mark_as_advanced(NNG_TRANSPORT_TCP)

# MQTT TCP transport
option (NNG_TRANSPORT_MQTT_TCP "Enable MQTT TCP transport." ON)
mark_as_advanced(NNG_TRANSPORT_MQTT_TCP)

# TLS transport
option (NNG_TRANSPORT_TLS "Enable TLS transport." ON)
mark_as_advanced(NNG_TRANSPORT_TLS)

# MQTT TLS transport
option (NNG_TRANSPORT_MQTT_TLS "Enable MQTT TLS transport." ON )
mark_as_advanced(NNG_TRANSPORT_MQTT_TLS)

# WebSocket
option (NNG_TRANSPORT_WS "Enable WebSocket transport." ON)
mark_as_advanced(NNG_TRANSPORT_WS)

CMAKE_DEPENDENT_OPTION(NNG_TRANSPORT_WSS "Enable WSS transport." ON
        "NNG_ENABLE_TLS" OFF)
mark_as_advanced(NNG_TRANSPORT_WSS)

option (NNG_TRANSPORT_FDC "Enable File Descriptor transport (EXPERIMENTAL)" ON)
mark_as_advanced(NNG_TRANSPORT_FDC)

# ZeroTier
option (NNG_TRANSPORT_ZEROTIER "Enable ZeroTier transport (requires libzerotiercore)." OFF)
mark_as_advanced(NNG_TRANSPORT_ZEROTIER)

if (NNG_TRANSPORT_WS OR NNG_TRANSPORT_WSS)
    # Make sure things we *MUST* have are enabled.
    set(NNG_SUPP_WEBSOCKET ON)
    set(NNG_SUPP_HTTP ON)
    set(NNG_SUPP_BASE64 ON)
    set(NNG_SUPP_SHA1 ON)
endif()

set(NNG_TRANSPORT_MQTT_QUIC ${NNG_ENABLE_QUIC})
