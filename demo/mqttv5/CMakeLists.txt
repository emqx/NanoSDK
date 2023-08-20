#
# This software is supplied under the terms of the MIT License, a
# copy of which should be located in the distribution where this
# file was obtained (LICENSE.txt).  A copy of the license may also be
# found online at https://opensource.org/licenses/MIT.

cmake_minimum_required (VERSION 2.8.12)

project(mqttv5_client)

find_package(nng CONFIG REQUIRED)

find_package(Threads)

add_executable(mqttv5_client mqttv5_client.c)
target_link_libraries(mqttv5_client nng::nng)
target_link_libraries(mqttv5_client ${CMAKE_THREAD_LIBS_INIT})

if(NNG_ENABLE_TLS)
    find_package(MbedTLS)
    target_link_libraries(mqttv5_client mbedtls mbedx509 mbedcrypto)
    add_definitions(-DNNG_SUPP_TLS)
endif()

if (NNG_ENABLE_SQLITE)
    add_definitions(-DNNG_SUPP_SQLITE)
    target_link_libraries(mqttv5_client dl)
endif (NNG_ENABLE_SQLITE)

target_compile_definitions(mqttv5_client PRIVATE NNG_ELIDE_DEPRECATED)
