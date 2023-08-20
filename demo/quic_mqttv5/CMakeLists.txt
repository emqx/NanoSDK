#
# Copyright 2018 Capitar IT Group BV <info@capitar.com>
# Copyright 2018 Staysail Systems, Inc. <info@staysail.tech>
#
# This software is supplied under the terms of the MIT License, a
# copy of which should be located in the distribution where this
# file was obtained (LICENSE.txt).  A copy of the license may also be
# found online at https://opensource.org/licenses/MIT.

cmake_minimum_required (VERSION 2.8.12)

project(quic_clientv5)

# Call this from your own project's makefile.
find_package(nng CONFIG REQUIRED)

find_package(Threads)
find_package(msquic)
find_package(OpenSSL)

add_executable(quic_client_v5 quic_client_v5.c)

if(OPENSSL_FOUND)
    include_directories(${OPENSSL_INCLUDE_DIRS})
    target_link_libraries(quic_client_v5 OpenSSL::Crypto OpenSSL::SSL)
endif()

target_link_libraries(quic_client_v5 nng::nng msquic pthread)
target_compile_definitions(quic_client_v5 PRIVATE NNG_ELIDE_DEPRECATED)

if (NNG_ENABLE_SQLITE)
    add_definitions(-DNNG_SUPP_SQLITE)
    target_link_libraries(quic_client dl)
endif (NNG_ENABLE_SQLITE)
