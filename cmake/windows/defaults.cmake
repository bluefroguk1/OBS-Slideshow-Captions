# CMake Windows defaults module

include_guard(GLOBAL)

# Enable find_package targets to become globally available targets
set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL TRUE)

include(buildspec)

# The vcpkg toolchain sets CMAKE_PREFIX_PATH as a normal variable. Merge the
# bootstrap's cached SDK prefixes into it so find_package can see both SDKs.
list(APPEND CMAKE_PREFIX_PATH $CACHE{CMAKE_PREFIX_PATH})
list(REMOVE_DUPLICATES CMAKE_PREFIX_PATH)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(
    CMAKE_INSTALL_PREFIX
    "$ENV{ALLUSERSPROFILE}/obs-studio/plugins"
    CACHE STRING
    "Default plugin installation directory"
    FORCE
  )
endif()
