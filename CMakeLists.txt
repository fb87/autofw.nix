cmake_minimum_required(VERSION 3.24)
project(shell_app LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# Build configuration.
#
#   OA_BACKEND   display backend        : wayland (default) | drm
#   OA_RENDERER  renderer               : gl (default) | software
#
# Supported combinations:
#   wayland+gl      Wayland + EGL/OpenGL ES
#   wayland+software Wayland + wl_shm CPU buffers
#   drm+gl          DRM/KMS + GBM + EGL/OpenGL ES
#   drm+software    DRM/KMS + CPU dumb buffers
# ---------------------------------------------------------------------------
set(OA_BACKEND "wayland" CACHE STRING "Display backend: wayland | drm")
set(OA_RENDERER "gl" CACHE STRING "Renderer: gl | software")

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
  ${FLUTTER_ENGINE_LIBRARY} PkgConfig::XKBCOMMON)

# --- backend selection -----------------------------------------------------
if(OA_BACKEND STREQUAL "wayland")
  pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client wayland-egl)
  target_compile_definitions(shell_impl INTERFACE SHELL_BACKEND_WAYLAND=1)
  target_link_libraries(shell_impl INTERFACE PkgConfig::WAYLAND)
elseif(OA_BACKEND STREQUAL "drm")
  find_path(GBM_INCLUDE_DIR NAMES gbm.h)
  find_library(GBM_LIBRARY NAMES gbm)
  find_path(DRM_MODESET_INCLUDE_DIR NAMES xf86drmMode.h)
  find_path(DRM_CORE_INCLUDE_DIR NAMES drm.h PATHS "${DRM_MODESET_INCLUDE_DIR}/.."
            PATH_SUFFIXES libdrm)
  find_library(DRM_LIBRARY NAMES drm libdrm)
  if(NOT GBM_LIBRARY OR NOT DRM_LIBRARY)
    message(FATAL_ERROR "drm backend requested but gbm/libdrm not found")
  endif()
  target_compile_definitions(shell_impl INTERFACE SHELL_BACKEND_DRM=1)
  target_include_directories(shell_impl INTERFACE
    ${GBM_INCLUDE_DIR} ${DRM_MODESET_INCLUDE_DIR} ${DRM_CORE_INCLUDE_DIR})
  target_link_libraries(shell_impl INTERFACE ${GBM_LIBRARY} ${DRM_LIBRARY})
else()
  message(FATAL_ERROR "Unknown OA_BACKEND '${OA_BACKEND}'")
endif()

# --- renderer selection ------------------------------------------------------
if(OA_RENDERER STREQUAL "gl")
  target_compile_definitions(shell_impl INTERFACE SHELL_RENDERER_GL=1)
  target_link_libraries(shell_impl INTERFACE ${EGL_LIBRARY} ${GLES2_LIBRARY})
elseif(OA_RENDERER STREQUAL "software")
  # no EGL/GLES needed
else()
  message(FATAL_ERROR "Unknown OA_RENDERER '${OA_RENDERER}'")
endif()

set(SHELL_APP_SOURCES src/main.cpp)
if(OA_BACKEND STREQUAL "wayland")
  # Wayland xdg-shell generated bindings.
  list(APPEND SHELL_APP_SOURCES include/shell/protocol/xdg-shell-protocol.c)
endif()
add_executable(shell_app ${SHELL_APP_SOURCES})
target_link_libraries(shell_app PRIVATE shell_impl ${CMAKE_DL_LIBS} pthread)

install(TARGETS shell_app RUNTIME DESTINATION bin)
