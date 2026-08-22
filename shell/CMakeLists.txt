cmake_minimum_required(VERSION 3.24)
project(flshell_app LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Flutter embedder library + headers.
if(NOT DEFINED FLUTTER_ENGINE_ROOT)
  if(DEFINED ENV{FLUTTER_ENGINE_ROOT})
    set(FLUTTER_ENGINE_ROOT "$ENV{FLUTTER_ENGINE_ROOT}")
  else()
    set(FLUTTER_ENGINE_ROOT "${CMAKE_SOURCE_DIR}/../result")
  endif()
endif()
find_path(FLUTTER_EMBEDDER_INCLUDE_DIR flutter_embedder.h
  PATHS "${FLUTTER_ENGINE_ROOT}/include")
find_library(FLUTTER_ENGINE_LIBRARY flutter_engine
  PATHS "${FLUTTER_ENGINE_ROOT}/lib")

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client wayland-egl)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
find_path(EGL_INCLUDE_DIR NAMES EGL/egl.h)
find_library(EGL_LIBRARY NAMES EGL libEGL)
find_path(GLES2_INCLUDE_DIR NAMES GLES2/gl2.h)
find_library(GLES2_LIBRARY NAMES GLESv2 libGLESv2)
add_compile_definitions(EGL_EGLEXT_PROTOTYPES)

# The header-only shell framework.
add_library(shell_impl INTERFACE)
target_include_directories(shell_impl INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${FLUTTER_EMBEDDER_INCLUDE_DIR} ${EGL_INCLUDE_DIR} ${GLES2_INCLUDE_DIR})
target_link_libraries(shell_impl INTERFACE
  ${FLUTTER_ENGINE_LIBRARY} ${EGL_LIBRARY} ${GLES2_LIBRARY}
  PkgConfig::WAYLAND PkgConfig::XKBCOMMON)

add_executable(flshell_app
  src/main.cpp
  include/shell/protocol/xdg-shell-protocol.c)
target_link_libraries(flshell_app PRIVATE shell_impl ${CMAKE_DL_LIBS} pthread)

install(TARGETS flshell_app RUNTIME DESTINATION bin)
