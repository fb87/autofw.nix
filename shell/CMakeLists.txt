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
# Flutter embedder library + headers. Provide the install/layout path via
# FLUTTER_ENGINE_ROOT (the flutter-engine output from our flake), or discover
# through pkg-config / an inherited environment.
# ---------------------------------------------------------------------------
if(NOT DEFINED FLUTTER_ENGINE_ROOT)
  if(DEFINED ENV{FLUTTER_ENGINE_ROOT})
    set(FLUTTER_ENGINE_ROOT "$ENV{FLUTTER_ENGINE_ROOT}")
  else()
    # Fall back to a conventional relative location produced by the flake.
    set(FLUTTER_ENGINE_ROOT "${CMAKE_SOURCE_DIR}/../result")
  endif()
endif()

find_path(FLUTTER_EMBEDDER_INCLUDE_DIR flutter_embedder.h
  PATHS "${FLUTTER_ENGINE_ROOT}/include")
find_library(FLUTTER_ENGINE_LIBRARY flutter_engine
  PATHS "${FLUTTER_ENGINE_ROOT}/lib")

# ---------------------------------------------------------------------------
# System dependencies
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)

pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client wayland-egl)

# EGL / OpenGLES2 (libglvnd).
find_path(EGL_INCLUDE_DIR NAMES EGL/egl.h)
find_library(EGL_LIBRARY NAMES EGL libEGL)
find_path(GLES2_INCLUDE_DIR NAMES GLES2/gl2.h)
find_library(GLES2_LIBRARY NAMES GLESv2 libGLESv2)

# Expose the extended EGL prototypes (eglGetPlatformDisplayEXT, ...).
add_compile_definitions(EGL_EGLEXT_PROTOTYPES)

# DRM/KMS backend dependencies (found directly; pkg-config for these tends to
# be unreliable depending on output layout).
find_path(GBM_INCLUDE_DIR NAMES gbm.h)
find_library(GBM_LIBRARY NAMES gbm)
find_path(DRM_MODESET_INCLUDE_DIR NAMES xf86drmMode.h)
# drm.h lives in a libdrm-specific subdirectory.
find_path(DRM_CORE_INCLUDE_DIR NAMES drm.h PATHS "${DRM_MODESET_INCLUDE_DIR}/.."
          PATH_SUFFIXES libdrm)
find_library(DRM_LIBRARY NAMES drm libdrm)

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
set(SOURCES
  src/main.cpp
  src/oa_engine.cpp
  src/oa_egl.cpp
  src/oa_wayland.cpp
  src/oa_kms.cpp
  src/protocol/xdg-shell-protocol.c
)

add_executable(openauto_shell ${SOURCES})

target_include_directories(openauto_shell PRIVATE
  "${FLUTTER_EMBEDDER_INCLUDE_DIR}"
  "${EGL_INCLUDE_DIR}"
  "${GLES2_INCLUDE_DIR}"
)

target_link_libraries(openauto_shell PRIVATE
  "${FLUTTER_ENGINE_LIBRARY}"
  "${EGL_LIBRARY}"
  "${GLES2_LIBRARY}"
  PkgConfig::WAYLAND
)

# Attach the DRM/KMS include/lib directories when present.
if(GBM_INCLUDE_DIR AND GBM_LIBRARY AND DRM_MODESET_INCLUDE_DIR AND
   DRM_CORE_INCLUDE_DIR AND DRM_LIBRARY)
  target_include_directories(openauto_shell PRIVATE
    "${GBM_INCLUDE_DIR}"
    "${DRM_MODESET_INCLUDE_DIR}"
    "${DRM_CORE_INCLUDE_DIR}")
  target_link_libraries(openauto_shell PRIVATE "${GBM_LIBRARY}" "${DRM_LIBRARY}")
  target_compile_definitions(openauto_shell PRIVATE OA_HAVE_KMS=1)
else()
  message(WARNING "DRM/KMS dependencies not found; building without them")
endif()

if(NOT TARGET_ARCH)
  target_link_libraries(openauto_shell PRIVATE ${CMAKE_DL_LIBS} pthread)
endif()

install(TARGETS openauto_shell RUNTIME DESTINATION bin)
