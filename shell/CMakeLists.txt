cmake_minimum_required(VERSION 3.16)
project(openauto_shell LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# Build configuration: select the display backend.
#   -DOA_BACKEND=wayland   (default) Wayland compositor
#   -DOA_BACKEND=kms       DRM/KMS + GBM
# ---------------------------------------------------------------------------
set(OA_BACKEND "wayland" CACHE STRING "Display backend: wayland | kms")

# Flutter embedder library + headers (FLUTTER_ENGINE_ROOT or conventional dir).
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

# System deps.
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client wayland-egl)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
find_path(EGL_INCLUDE_DIR NAMES EGL/egl.h)
find_library(EGL_LIBRARY NAMES EGL libEGL)
find_path(GLES2_INCLUDE_DIR NAMES GLES2/gl2.h)
find_library(GLES2_LIBRARY NAMES GLESv2 libGLESv2)
add_compile_definitions(EGL_EGLEXT_PROTOTYPES)

# DRM deps (only needed for the KMS backend).
find_path(GBM_INCLUDE_DIR NAMES gbm.h)
find_library(GBM_LIBRARY NAMES gbm)
find_path(DRM_MODESET_INCLUDE_DIR NAMES xf86drmMode.h)
find_path(DRM_CORE_INCLUDE_DIR NAMES drm.h PATHS "${DRM_MODESET_INCLUDE_DIR}/.."
          PATH_SUFFIXES libdrm)
find_library(DRM_LIBRARY NAMES drm libdrm)

# ---------------------------------------------------------------------------
# Core: header-only interface library (backend-agnostic common logic).
# ---------------------------------------------------------------------------
add_library(openauto_core INTERFACE)
target_include_directories(openauto_core INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${FLUTTER_EMBEDDER_INCLUDE_DIR} ${EGL_INCLUDE_DIR} ${GLES2_INCLUDE_DIR})
target_link_libraries(openauto_core INTERFACE
  ${FLUTTER_ENGINE_LIBRARY} ${EGL_LIBRARY} ${GLES2_LIBRARY}
  PkgConfig::WAYLAND PkgConfig::XKBCOMMON)

# ---------------------------------------------------------------------------
# The executable (source list depends on the selected backend).
# ---------------------------------------------------------------------------
set(SOURCES src/main.cpp src/protocol/xdg-shell-protocol.c)

if(OA_BACKEND STREQUAL "wayland")
  target_compile_definitions(openauto_core INTERFACE OA_BACKEND_WAYLAND=1)
elseif(OA_BACKEND STREQUAL "kms")
  target_compile_definitions(openauto_core INTERFACE OA_BACKEND_KMS=1)
  if(NOT GBM_LIBRARY OR NOT DRM_LIBRARY)
    message(FATAL_ERROR "KMS backend requested but gbm/libdrm not found")
  endif()
  target_include_directories(openauto_core INTERFACE
    ${GBM_INCLUDE_DIR} ${DRM_MODESET_INCLUDE_DIR} ${DRM_CORE_INCLUDE_DIR})
  target_link_libraries(openauto_core INTERFACE ${GBM_LIBRARY} ${DRM_LIBRARY})
else()
  message(FATAL_ERROR "Unknown OA_BACKEND '${OA_BACKEND}'")
endif()

add_executable(openauto_shell ${SOURCES})
target_link_libraries(openauto_shell PRIVATE openauto_core ${CMAKE_DL_LIBS} pthread)

install(TARGETS openauto_shell RUNTIME DESTINATION bin)
