# Local development against matching OBS source headers and an installed OBS app.
set(OBS_MACOS_SOURCE "" CACHE PATH "OBS source checkout matching OBS_MACOS_APP")
find_path(OBS_SIMDE_INCLUDE_DIR NAMES simde/x86/sse2.h REQUIRED)
set(_obs_frameworks "${OBS_MACOS_APP}/Contents/Frameworks")
set(_obs_library "${_obs_frameworks}/libobs.framework/Versions/A/libobs")
set(_obs_headers "${OBS_MACOS_SOURCE}/libobs")
if(NOT EXISTS "${_obs_library}" OR NOT EXISTS "${_obs_headers}/obsconfig.h.in")
  message(FATAL_ERROR "Set OBS_MACOS_APP and OBS_MACOS_SOURCE to an OBS app and matching source checkout.")
endif()
if(ENABLE_FRONTEND_API OR ENABLE_QT)
  message(FATAL_ERROR "The installed-app SDK shortcut supports the libobs-only build.")
endif()

include(compiler_common)
set(OBS_RELEASE_CANDIDATE 0)
set(OBS_BETA 0)
configure_file("${_obs_headers}/obsconfig.h.in" "${CMAKE_CURRENT_BINARY_DIR}/obs-sdk/obsconfig.h" @ONLY)
add_library(OBS::libobs SHARED IMPORTED)
set_target_properties(
  OBS::libobs
  PROPERTIES
    IMPORTED_LOCATION "${_obs_library}"
    INTERFACE_INCLUDE_DIRECTORIES "${_obs_headers};${CMAKE_CURRENT_BINARY_DIR}/obs-sdk;${OBS_SIMDE_INCLUDE_DIR}"
)
set(CMAKE_BUILD_RPATH "${_obs_frameworks}")
set(CMAKE_INSTALL_RPATH "@executable_path/../Frameworks")
