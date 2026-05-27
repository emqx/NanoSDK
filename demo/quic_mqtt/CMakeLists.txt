#
# Copyright 2018 Capitar IT Group BV <info@capitar.com>
# Copyright 2018 Staysail Systems, Inc. <info@staysail.tech>
#
# This software is supplied under the terms of the MIT License, a
# copy of which should be located in the distribution where this
# file was obtained (LICENSE.txt).  A copy of the license may also be
# found online at https://opensource.org/licenses/MIT.

cmake_minimum_required(VERSION 3.13)

project(quic_client)

# Compatibility shim for local scripts that pass a custom install prefix.
if (CMAKE_REPFIX_PATH)
  list(APPEND CMAKE_PREFIX_PATH "${CMAKE_REPFIX_PATH}")
  if (NOT nng_DIR)
    if (EXISTS "${CMAKE_REPFIX_PATH}/lib64/cmake/nng")
      set(nng_DIR "${CMAKE_REPFIX_PATH}/lib64/cmake/nng")
    elseif (EXISTS "${CMAKE_REPFIX_PATH}/lib/cmake/nng")
      set(nng_DIR "${CMAKE_REPFIX_PATH}/lib/cmake/nng")
    endif ()
  endif ()
  if (NOT msquic_DIR AND EXISTS "${CMAKE_REPFIX_PATH}/share/msquic")
    set(msquic_DIR "${CMAKE_REPFIX_PATH}/share/msquic")
  endif ()
endif ()

if (BUILD_DEMO)
else ()
# Call this from your own project's makefile.
  find_package(nng CONFIG REQUIRED)
endif (BUILD_DEMO)

find_package(Threads)
if (NOT TARGET msquic AND NOT TARGET nng::msquic)
  find_package(msquic CONFIG)
endif ()

if (DEBUG)
  SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g")
  if (ASAN)
    SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address")
  endif (ASAN)
  if (TSAN)
    SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=thread")
  endif (TSAN)
endif (DEBUG)

add_executable(quic_client  quic_client.c)

add_definitions(-DSUPP_QUIC)

if (TARGET nng::nng)
  set(_NNG_TARGET nng::nng)
else ()
  set(_NNG_TARGET nng)
endif ()

target_link_libraries(quic_client PRIVATE
  ${_NNG_TARGET}
  Threads::Threads)

if (TARGET nng::msquic)
  target_link_libraries(quic_client PRIVATE nng::msquic)
elseif (TARGET msquic)
  target_link_libraries(quic_client PRIVATE msquic)
endif ()
target_compile_definitions(quic_client PRIVATE NNG_ELIDE_DEPRECATED)

if (NNG_ENABLE_SQLITE)
    add_definitions(-DNNG_SUPP_SQLITE)
    target_link_libraries(quic_client dl)
endif (NNG_ENABLE_SQLITE)

