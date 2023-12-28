#
# Copyright 2023 NanoMQ Team, Inc. <jaylin@emqx.io> //
# This software is supplied under the terms of the MIT License, a
# copy of which should be located in the distribution where this
# file was obtained (LICENSE.txt).  A copy of the license may also be
# found online at https://opensource.org/licenses/MIT.
#

nng_sources(file.c log.c)
nng_headers(nng/supplemental/nanolib/file.h nng/supplemental/nanolib/log.h)
