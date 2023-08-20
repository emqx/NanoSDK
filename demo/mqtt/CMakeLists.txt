#
# This software is supplied under the terms of the MIT License, a
# copy of which should be located in the distribution where this
# file was obtained (LICENSE.txt).  A copy of the license may also be
# found online at https://opensource.org/licenses/MIT.

cmake_minimum_required (VERSION 2.8.12)

project(mqtt_client)

find_package(nng CONFIG REQUIRED)

find_package(Threads)

add_executable(mqtt_client mqtt_client.c)
target_link_libraries(mqtt_client nng::nng)
target_link_libraries(mqtt_client ${CMAKE_THREAD_LIBS_INIT})

if(NNG_ENABLE_TLS)
    find_package(MbedTLS)
    target_link_libraries(mqtt_client mbedtls mbedx509 mbedcrypto)
    add_definitions(-DNNG_SUPP_TLS)
endif()

if(NNG_ENABLE_SQLITE)
    add_definitions(-DNNG_SUPP_SQLITE)
    target_link_libraries(mqtt_client dl)
endif(NNG_ENABLE_SQLITE)

target_compile_definitions(mqtt_client PRIVATE NNG_ELIDE_DEPRECATED)
