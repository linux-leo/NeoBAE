#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "OpusFile::opusfile" for configuration "Release"
set_property(TARGET OpusFile::opusfile APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(OpusFile::opusfile PROPERTIES
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libopusfile.so"
  IMPORTED_SONAME_RELEASE "libopusfile.so"
  )

list(APPEND _cmake_import_check_targets OpusFile::opusfile )
list(APPEND _cmake_import_check_files_for_OpusFile::opusfile "${_IMPORT_PREFIX}/lib/libopusfile.so" )

# Import target "OpusFile::opusurl" for configuration "Release"
set_property(TARGET OpusFile::opusurl APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(OpusFile::opusurl PROPERTIES
  IMPORTED_LINK_DEPENDENT_LIBRARIES_RELEASE "OpusFile::opusfile"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libopusurl.so"
  IMPORTED_SONAME_RELEASE "libopusurl.so"
  )

list(APPEND _cmake_import_check_targets OpusFile::opusurl )
list(APPEND _cmake_import_check_files_for_OpusFile::opusurl "${_IMPORT_PREFIX}/lib/libopusurl.so" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
