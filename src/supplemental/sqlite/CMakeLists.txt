#
# Copyright 2020 Staysail Systems, Inc. <info@staysail.tech>
#
# This software is supplied under the terms of the MIT License, a
# copy of which should be located in the distribution where this
# file was obtained (LICENSE.txt).  A copy of the license may also be
# found online at https://opensource.org/licenses/MIT.
#
if (NNG_ENABLE_SQLITE)
  nng_sources(sqlite3.c sqlite3.h)

  if (NNG_PLATFORM_POSIX)
    nng_link_libraries(dl)
  elseif (NNG_PLATFORM_WINDOWS)
    nng_link_libraries(ws2_32)
  endif()

  nng_test(sqlite3_test)
endif (NNG_ENABLE_SQLITE)


