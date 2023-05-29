#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "msquic" for configuration "Release"
set_property(TARGET msquic APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(msquic PROPERTIES
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libmsquic.so.2.1.8"
  IMPORTED_SONAME_RELEASE "libmsquic.so.2"
  )

list(APPEND _IMPORT_CHECK_TARGETS msquic )
list(APPEND _IMPORT_CHECK_FILES_FOR_msquic "${_IMPORT_PREFIX}/lib/libmsquic.so.2.1.8" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
