
[![Discord](https://img.shields.io/discord/931086341838622751?label=Discord&logo=discord)](https://discord.gg/xYGf3fQnES)
[![Twitter](https://img.shields.io/badge/Follow-EMQ-1DA1F2?logo=twitter)](https://twitter.com/EMQTech)
[![YouTube](https://img.shields.io/badge/Subscribe-EMQ-FF0000?logo=youtube)](https://www.youtube.com/channel/UC5FjR77ErAxvZENEWzQaO5Q)
[![Community](https://img.shields.io/badge/Community-NanoMQ-yellow?logo=github)](https://github.com/emqx/nanomq/discussions)
[![Build Status](https://img.shields.io/github/actions/workflow/status/emqx/NanoSDK/build_packages.yaml?branch=main&label=Build)](https://github.com/emqx/NanoSDK/actions)

# NanoSDK C Client Library for the MQTT Protocol

This repository contains the source code for the NanoSDK MQTT C client library.

This project is a wrapping of the nanomsg-NNG(https://github.com/nanomsg/nng) 
nanomsg known as https://github.com/nanomsg/nanomsg[libnanomsg],
and adds significant new capabilities like MQTT & QUIC while retaining
compatibility with the original Scalability Protocols.

NOTE: In honor of NNG & Garrett(Creator & Maintainer), they are the cornerstone of NanoSDK. We respect all his contributions to the Open-Source world. And We create this repository only for tracking Issues/Discussions and fast-paced development while the MQTT support is missing in upstream.


# Introduction & Rationale

NanoSDK is an open-source project jointly developed by EMQ and NNG. NanoSDK as a repository, it is maintained independently as the NNG version of NanoMQ (https://github.com/emqx/nanomq). At present, its master branch is a special version developed and optimized for MQTT workload and maintained independently, from which we have submitted several valuable PRs and Issues for NNG.
Therefore, to make NanoMQ compatible with NNG's SP protocol, we are committed to adding MQTT 3.1.1/5.0 protocol support for NNG at here. So that the two kinds of protocols(MQTT and SP) can be used together with NanoSDK. This is also in accordance with the RoadMap direction previously formulated by NanoMQ team.
According to the technical goals previously formulated with the NNG project maintainer - Garrett at the jointly open-source cooperation meeting, NNG will support ZeroMQ and MQTT 3.1.1/5.0 in the future. Before this really happens, this project will be maintained independently. The internal design of NanoSDK honors the programming style of the NNG framework and is compatible with the original SP protocol of NNG. At the same time, it does not affect the other features like HTTP/Websocket/TLS.

## Advantages
Compared with other popular MQTT 3.1.1 SDK, NanoSDK has the following advantages:
1. Fully asynchronous I/O and good SMP support
NanoSDK based on NNG's asynchronous I/O, we implement the Actor-like programming model in C language. And managing to distribute the computation load among multiple CPU cores averagely.
2. High compatibility and portability
Inheriting the compatibility and easy portability of NNG, NanoSDK only relies on the native POSIX standard API and is friendly to various distributions of Linux. It can be migrated to any hardware and operating system platform easily.
3. Support multiple API styles
The programming style of the NNG framework comes with a high learning cost, and users need to have a deep understanding of the concept of AIO and Context. Therefore, we also prepared a traditional callback registration mechanism for users who are accustomed to using Paho and Mosquitto SDK. This not only reduces the programming difficulty but also reserves the advantages of NNG.
4. High throughput & Low latency
In NanoMQ's test report, its performance advantages of powerful high throughput and low latency have been reflected, and the direct successor of NanoMQ - the SDK also has excellent performance. It is cost-effective in terms of resources consumption. Unlike the traditional MQTT SDK which has only 1-2 threads, NanoSDK can make full use of system hardware resources to provide higher consumption throughput.
In most IoT solutions based on EMQX, the backend service’s insufficient consuming capability always results in message congestion, which has always been a problem for open source developers. Especially for QoS 1/2 messages, most SDKs reply Ack for QoS 1/2 messages synchronously. NanoSDK provides asynchronous Ack capability under the premise of ensuring the QoS message sequence and message retransmission mechanism, which greatly improves the throughput and consumption capacity of QoS 1/2.

Inherited from NNG, NanoSDK is a performant & higly portable SDK. Which enable users create client application with high throughput.  

This code builds libraries which enable applications to connect to an [MQTT](http://mqtt.org) broker to publish messages, and to subscribe to topics and receive published messages.

For MQTT broker introduction, please refer to EMQX (https://github.com/emqx/emqx)

## Information About MQTT

* [MQTT website](http://mqtt.org)
* [The MQTT 3.1.1 standard](http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html)
* [The MQTT 5.0 standard](https://docs.oasis-open.org/mqtt/mqtt/v5.0/mqtt-v5.0.html)
* [OASIS Introduction to MQTT presentation](https://www.oasis-open.org/committees/download.php/49205/MQTT-OASIS-Webinar.pdf)

## Libraries

The NanoSDK C client comprises four variant libraries, shared or static:

 * nng/demo/mqtt- simple mqtt client
 * nng/demo/mqtt_async - asynchronous MQTT client & command-linewith SSL/TLS (MQTTAsync)
 * nng/demo/mqttv5- simple mqttv5 client with async API
 * nng/demo/quic- simple MQTT over QUIC demo (Attention: MsQUIC needs to be installed)

## API Documentation

The API documentation is provided in Asciidoc format in the
`docs/man` subdirectory
The <<docs/man/libnng.3.adoc#,libnng(3)>> page is a good starting point.

In addition, many users would like to add self-defined behavior in the callback of online and offline. Therefore, we modified the callback method of connection and disconnection to allow users to perform blocking and waiting operations in the callback without affecting the MQTT connection & pradigm itself, so as to improve the flexibility of NanoSDK.
However, it should be noted that this will consume the number of threads inside the NanoSDK. If the taskq thread is exhausted, it will still affect the operation of the entire client. Please use it cautiously.

## Building with CMake

  ```bash
  git clone https://github.com/emqx/NanoSDK ; cd NanoSDK
  git submodule update --init --recursive 
  mkdir build && cd build
  cmake -G Ninja ..
  ninja
  ```
To enable MQTT over QUIC feature  
```bash
-DNNG_ENABLE_QUIC=ON
```

