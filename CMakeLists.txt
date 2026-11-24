cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

set(OBS_MACOS_APP "" CACHE PATH "Build against this installed OBS.app instead of downloading the OBS SDK")
if(APPLE AND OBS_MACOS_APP)
  include(macos-app-sdk)
else()
  include(compilerconfig)
  include(defaults)
endif()
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

if(NOT TARGET OBS::libobs)
  find_package(libobs REQUIRED)
endif()
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 REQUIRED COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE "$<$<COMPILE_LANG_AND_ID:C,Clang,AppleClang>:-Wno-quoted-include-in-framework-header;-Wno-comma>"
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

find_package(exiv2 CONFIG REQUIRED)
if(exiv2_VERSION VERSION_LESS 0.28)
  message(FATAL_ERROR "Slideshow Caption requires Exiv2 0.28 or newer.")
endif()
find_package(PCRE2 CONFIG QUIET COMPONENTS 8BIT)
if(NOT TARGET PCRE2::8BIT)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(PCRE2 REQUIRED IMPORTED_TARGET libpcre2-8)
  add_library(PCRE2::8BIT ALIAS PkgConfig::PCRE2)
endif()

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Exiv2::exiv2lib PCRE2::8BIT)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE PCRE2_CODE_UNIT_WIDTH=8)
target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.c src/slideshow-caption.cpp src/caption-content.cpp)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
