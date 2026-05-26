#
# This software is supplied under the terms of the MIT License, a
# copy of which should be located in the distribution where this
# file was obtained (LICENSE.txt).  A copy of the license may also be
# found online at https://opensource.org/licenses/MIT.
#

#  MQTT protocol
nng_directory(mqtt)

nng_sources_if(
    NNG_PROTO_MQTT_CLIENT
    mqtt_client.c
    sqlite_handler.c
    sqlite_handler.h
)
nng_headers_if(NNG_PROTO_MQTT_CLIENT nng/mqtt/mqtt_client.h)
nng_defines_if(NNG_PROTO_MQTT_CLIENT NNG_HAVE_MQTT_CLIENT)
message(" Check MQTT_QUIC_CLIENT support: ${NNG_PROTO_MQTT_QUIC_CLIENT} ")
if (NNG_PROTO_MQTT_QUIC_CLIENT)
    nng_sources_if(NNG_PROTO_MQTT_QUIC_CLIENT mqtt_quic_client.c)
    nng_headers_if(NNG_PROTO_MQTT_QUIC_CLIENT nng/mqtt/mqtt_quic_client.h)
    nng_defines_if(NNG_PROTO_MQTT_QUIC_CLIENT NNG_HAVE_MQTT_QUIC_CLIENT)
endif()
