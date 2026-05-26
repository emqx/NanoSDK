#
# Copyright 2020 Staysail Systems, Inc. <info@staysail.tech>
# Copyright 2018 Capitar IT Group BV <info@capitar.com>
#
# This software is supplied under the terms of the MIT License, a
# copy of which should be located in the distribution where this
# file was obtained (LICENSE.txt).  A copy of the license may also be
# found online at https://opensource.org/licenses/MIT.
#

# QUIC protocol
### Testing ???
# nng_directory(mqtt)

message(" Check MQTT_QUIC_TRANSPORT support: ${NNG_TRANSPORT_MQTT_QUIC} ")

if (NNG_TRANSPORT_MQTT_QUIC)
    nng_sources_if(NNG_TRANSPORT_MQTT_QUIC mqtt_quic.c)
    nng_headers_if(NNG_TRANSPORT_MQTT_QUIC nng/mqtt/transport/quic/mqtt_quic.h)
    nng_defines_if(NNG_TRANSPORT_MQTT_QUIC NNG_TRANSPORT_MQTT_QUIC)
endif()
