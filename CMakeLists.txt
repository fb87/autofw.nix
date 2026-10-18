cmake_minimum_required(VERSION 3.24)
project(shell_app LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
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
# GBM is needed by shell::gbm/shell::child_renderer (process isolation,
# Step 4) regardless of display backend -- a child app's off-screen
# renderer opens /dev/dri/renderD128 directly, decoupled from whichever
# backend the primary display uses. Probed here unconditionally; linked
# below under OA_RENDERER=gl (it's meaningless without GL to render into
# the buffers it allocates).
find_path(GBM_INCLUDE_DIR NAMES gbm.h)
find_library(GBM_LIBRARY NAMES gbm)
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
  # Must come from a compiler flag, not a header #define: shell/egl.hpp
  # (included before shell/backend/wayland.hpp via shell/engine.hpp) is the
  # first thing to pull in <EGL/egl.h>, and its include guard means any
  # #define placed later in the translation unit is too late to affect
  # EGLNativeWindowType's definition in EGL/eglplatform.h.
  target_compile_definitions(shell_impl INTERFACE WL_EGL_PLATFORM=1)
  target_link_libraries(shell_impl INTERFACE PkgConfig::WAYLAND)
elseif(OA_BACKEND STREQUAL "drm")
  find_path(DRM_MODESET_INCLUDE_DIR NAMES xf86drmMode.h)
  find_path(DRM_CORE_INCLUDE_DIR NAMES drm.h PATHS "${DRM_MODESET_INCLUDE_DIR}/.."
            PATH_SUFFIXES libdrm)
  find_library(DRM_LIBRARY NAMES drm libdrm)
  if(NOT GBM_LIBRARY OR NOT DRM_LIBRARY)
    message(FATAL_ERROR "drm backend requested but gbm/libdrm not found")
  endif()
  # DRM backend needs GBM for its own scanout swapchain
  # (gbm_surface_create et al, backend/drm.hpp) regardless of renderer --
  # even drm+software allocates its front buffers through GBM. Linked here
  # unconditionally on OA_BACKEND; the gl-renderer branch below links it
  # again (harmlessly redundant when OA_BACKEND is also "drm") for
  # shell::child_renderer's unrelated, backend-independent need (Step 4).
  target_include_directories(shell_impl INTERFACE ${GBM_INCLUDE_DIR})
  target_link_libraries(shell_impl INTERFACE ${GBM_LIBRARY})
  # DRM has no compositor of its own to supply input, so it reads directly
  # from libinput (M18).
  pkg_check_modules(LIBINPUT REQUIRED IMPORTED_TARGET libinput)
  pkg_check_modules(LIBUDEV REQUIRED IMPORTED_TARGET libudev)
  target_compile_definitions(shell_impl INTERFACE SHELL_BACKEND_DRM=1)
  target_include_directories(shell_impl INTERFACE
    ${DRM_MODESET_INCLUDE_DIR} ${DRM_CORE_INCLUDE_DIR})
  target_link_libraries(shell_impl INTERFACE ${DRM_LIBRARY}
    PkgConfig::LIBINPUT PkgConfig::LIBUDEV)
else()
  message(FATAL_ERROR "Unknown OA_BACKEND '${OA_BACKEND}'")
endif()

# --- renderer selection ------------------------------------------------------
if(OA_RENDERER STREQUAL "gl")
  if(NOT GBM_LIBRARY)
    message(FATAL_ERROR "gl renderer requested but gbm not found (needed by shell::child_renderer, Step 4)")
  endif()
  target_compile_definitions(shell_impl INTERFACE SHELL_RENDERER_GL=1)
  target_include_directories(shell_impl INTERFACE ${GBM_INCLUDE_DIR})
  target_link_libraries(shell_impl INTERFACE ${EGL_LIBRARY} ${GLES2_LIBRARY} ${GBM_LIBRARY})
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

# ---------------------------------------------------------------------------
# Examples & tests
# ---------------------------------------------------------------------------
option(SHELL_BUILD_EXAMPLES "Build shell examples" ON)
option(SHELL_BUILD_TESTS "Build shell tests" ON)

if(SHELL_BUILD_EXAMPLES)
  add_executable(shell_minimal examples/minimal/main.cpp)
  target_link_libraries(shell_minimal PRIVATE shell_impl)
endif()

if(SHELL_BUILD_TESTS)
  enable_testing()
  add_executable(shell_headers_test tests/headers/main.cpp)
  target_link_libraries(shell_headers_test PRIVATE shell_impl)
  add_test(NAME shell.headers COMMAND shell_headers_test)
endif()

install(TARGETS shell_app RUNTIME DESTINATION bin)
