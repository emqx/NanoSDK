#
# Copyright 2018 Capitar IT Group BV <info@capitar.com>
# Copyright 2018 Staysail Systems, Inc. <info@staysail.tech>
#
# This software is supplied under the terms of the MIT License, a
# copy of which should be located in the distribution where this
# file was obtained (LICENSE.txt).  A copy of the license may also be
# found online at https://opensource.org/licenses/MIT.

cmake_minimum_required (VERSION 3.22.1)

project(quic_clientv5)

set(msquic_DIR "share/msquic")
set(nng_DIR "lib/cmake/nng")

# Call this from your own project's makefile.
find_package(nng CONFIG REQUIRED)

find_package(Threads)
find_package(msquic)
find_package(OpenSSL)

include_directories(include)
link_directories(lib)

file(GLOB all_SRCS
        "${PROJECT_SOURCE_DIR}/include/*.h"
        "${PROJECT_SOURCE_DIR}/include/*.hpp"
        "${PROJECT_SOURCE_DIR}/src/*.cpp"
        "${PROJECT_SOURCE_DIR}/src/*.c")


add_executable(quic_client_v5 src/quic_client_v5.c src/the_caller.c)

if(OPENSSL_FOUND)
    include_directories(${OPENSSL_INCLUDE_DIRS})
    target_link_libraries(quic_client_v5 OpenSSL::Crypto OpenSSL::SSL)
endif()

target_compile_definitions(quic_client_v5 PRIVATE NNG_ELIDE_DEPRECATED)

target_link_libraries(quic_client_v5 nng msquic pthread)

