#
# This software is supplied under the terms of the MIT License, a
# copy of which should be located in the distribution where this
# file was obtained (LICENSE.txt).  A copy of the license may also be
# found online at https://opensource.org/licenses/MIT.
#

if (NNG_ENABLE_SCRAM)
    nng_sources(scram.c scram.h)
    nng_test(scram_test)
endif ()
