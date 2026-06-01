cmake_minimum_required(VERSION 3.13)

project(mqtt_full_client C)

set(MQTT_FULL_TARGET mqtt_full_client)

if (BUILD_DEMO)
else ()
  find_package(nng CONFIG REQUIRED)
endif ()

find_package(Threads REQUIRED)

add_executable(
  ${MQTT_FULL_TARGET}
  mqtt_full.c
  mqtt_full_config.c
  mqtt_full_runtime.c
)

set_target_properties(${MQTT_FULL_TARGET} PROPERTIES OUTPUT_NAME mqtt_client)

if (TARGET nng::nng)
  set(_NNG_TARGET nng::nng)
else ()
  set(_NNG_TARGET nng)
endif ()

target_link_libraries(${MQTT_FULL_TARGET} PRIVATE ${_NNG_TARGET} Threads::Threads)
target_compile_definitions(${MQTT_FULL_TARGET} PRIVATE NNG_ELIDE_DEPRECATED)

if (NNG_ENABLE_TLS)
  target_compile_definitions(${MQTT_FULL_TARGET} PRIVATE NNG_SUPP_TLS)
  find_package(MbedTLS QUIET)
  if (MbedTLS_FOUND)
    target_link_libraries(${MQTT_FULL_TARGET} PRIVATE mbedtls mbedx509 mbedcrypto)
  endif ()
endif ()

if (NNG_ENABLE_SQLITE)
  target_compile_definitions(${MQTT_FULL_TARGET} PRIVATE NNG_SUPP_SQLITE)
  if (UNIX AND NOT APPLE)
    target_link_libraries(${MQTT_FULL_TARGET} PRIVATE dl)
  elseif (WIN32)
    target_link_libraries(${MQTT_FULL_TARGET} PRIVATE ws2_32)
  endif ()
endif ()

# Enable QUIC code path only when the nng package exports a msquic target.
if (TARGET nng::msquic)
  target_compile_definitions(${MQTT_FULL_TARGET} PRIVATE SUPP_QUIC)
  target_link_libraries(${MQTT_FULL_TARGET} PRIVATE nng::msquic)
elseif (TARGET msquic)
  target_compile_definitions(${MQTT_FULL_TARGET} PRIVATE SUPP_QUIC)
  target_link_libraries(${MQTT_FULL_TARGET} PRIVATE msquic)
endif ()
