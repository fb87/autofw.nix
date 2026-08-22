// oa_kms.h -- DRM/KMS display backend. Header-only.
//
// Namespace-based functional backend (no classes). Opens a DRM primary node,
// configures a connector/CRTC, and renders into a GBM-backed EGL surface.
// Vsync is driven by DRM vblank waits. Exposes itself through `backend_api`.
//
// Copyright 2026 OpenAuto Framework.
#pragma once

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/errno.h>
#include <time.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>

#include "core/oa_backend.h"
#include "core/oa_egl.h"
#include "core/oa_types.h"

namespace oa::backend::kms {

// Opaque state for the DRM/KMS backend.
struct kms_ctx {
  int fd = -1;
  uint32_t crtc_id = 0;
  uint32_t connector_id = 0;
  drmModeModeInfo mode{};

  gbm_device* gbm_dev = nullptr;
  gbm_surface* gbm_surf = nullptr;

  void* egl_display = nullptr;
  void* egl_config = nullptr;
  void* egl_surface = nullptr;

  uint32_t width = 0;
  uint32_t height = 0;
  double dpi = 1.0;

  bool running = false;

  ::oa::backend::vsync_fn on_vsync;
  ::oa::backend::input_fn on_input;
};

inline bool open_device(kms_ctx* c) {
  static const char* cand[] = {"/dev/dri/card0", "/dev/dri/card1",
                               "/dev/dri/renderD128", nullptr};
  for (int i = 0; cand[i]; ++i) {
    int fd = open(cand[i], O_RDWR | O_CLOEXEC);
    if (fd < 0) continue;
    drmVersionPtr ver = drmGetVersion(fd);
    if (ver) {
      drmFreeVersion(ver);
      c->fd = fd;
      return true;
    }
    close(fd);
  }
  std::fprintf(stderr, "[oa-kms] no usable DRM device found\n");
  return false;
}

inline bool setup_kms(kms_ctx* c) {
  drmModeResPtr res = drmModeGetResources(c->fd);
  if (!res) return false;
  for (int i = 0; i < res->count_connectors; ++i) {
    drmModeConnector* conn = drmModeGetConnector(c->fd, res->connectors[i]);
    if (!conn) continue;
    if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
      c->connector_id = conn->connector_id;
      drmModeModeInfo* best = &conn->modes[0];
      for (int m = 0; m < conn->count_modes; ++m)
        if (conn->modes[m].type & DRM_MODE_TYPE_PREFERRED) best = &conn->modes[m];
      c->width = best->hdisplay;
      c->height = best->vdisplay;
      memcpy(&c->mode, best, sizeof(c->mode));
      for (int crtc = 0; crtc < res->count_crtcs; ++crtc) {
        if (conn->encoder_id) {
          drmModeEncoderPtr enc = drmModeGetEncoder(c->fd, conn->encoder_id);
          if (enc && (enc->possible_crtcs & (1 << crtc))) c->crtc_id = res->crtcs[crtc];
          if (enc) drmModeFreeEncoder(enc);
        }
      }
      drmModeFreeConnector(conn);
      break;
    }
    drmModeFreeConnector(conn);
  }
  drmModeFreeResources(res);
  return c->connector_id && c->crtc_id;
}

inline bool setup_egl(kms_ctx* c) {
  c->gbm_dev = gbm_create_device(c->fd);
  if (!c->gbm_dev) return false;
  c->gbm_surf = gbm_surface_create(c->gbm_dev, c->width, c->height,
                                   GBM_FORMAT_XRGB8888,
                                   GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
  if (!c->gbm_surf) return false;
  static const EGLAttrib kAttribs[] = {EGL_NONE};
  c->egl_display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, (void*)c->gbm_dev,
                                         kAttribs);
  if (!c->egl_display || eglInitialize((EGLDisplay)c->egl_display, nullptr,
                                       nullptr) != EGL_TRUE)
    return false;
  if (!egl::choose_config((EGLDisplay)c->egl_display, (EGLConfig*)&c->egl_config))
    return false;
  c->egl_surface = eglCreatePlatformWindowSurface(
      (EGLDisplay)c->egl_display, (EGLConfig)c->egl_config, (void*)c->gbm_surf,
      nullptr);
  return c->egl_surface != EGL_NO_SURFACE;
}

inline bool wait_for_vblank(int fd, frame_tick* tick);  // forward

// ---- backend API -----------------------------------------------------------
inline bool init(void* state) {
  auto* c = static_cast<kms_ctx*>(state);
  return open_device(c) && setup_kms(c) && setup_egl(c);
}

inline void run_loop(void* state, ::oa::backend::vsync_fn on_vsync,
                     ::oa::backend::input_fn on_input, ::oa::backend::resize_fn,
                     ::oa::backend::keymap_fn) {
  auto* c = static_cast<kms_ctx*>(state);
  c->on_vsync = std::move(on_vsync);
  c->on_input = std::move(on_input);
  c->running = true;
  while (c->running) {
    frame_tick tick;
    if (!wait_for_vblank(c->fd, &tick)) break;
    if (c->on_vsync) c->on_vsync(tick);
  }
  c->running = false;
}
inline void request_exit(void* state) { static_cast<kms_ctx*>(state)->running = false; }
inline void* egl_surface(void* state) { return static_cast<kms_ctx*>(state)->egl_surface; }
inline void* egl_display(void* state) { return static_cast<kms_ctx*>(state)->egl_display; }
inline void* egl_config(void* state) { return static_cast<kms_ctx*>(state)->egl_config; }
inline uint32_t width(const void* state) { return static_cast<const kms_ctx*>(state)->width; }
inline uint32_t height(const void* state) { return static_cast<const kms_ctx*>(state)->height; }
inline double dpi(const void* state) { return static_cast<const kms_ctx*>(state)->dpi; }

inline bool wait_for_vblank(int fd, frame_tick* tick) {
  drmVBlank vblank = {};
  vblank.request.type = DRM_VBLANK_RELATIVE;
  vblank.request.sequence = 1;
  if (drmWaitVBlank(fd, &vblank) < 0) return false;
  if (tick) {
    struct timespec ts;
    uint64_t ns = 0;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
      ns = (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
    tick->present_ns = ns;
    tick->target_ns = ns + 16666667ull;
  }
  return true;
}

inline bool flush_framebuffer(void* state) {
  auto* c = static_cast<kms_ctx*>(state);
  EGLDisplay dpy = (EGLDisplay)c->egl_display;
  if (eglSwapBuffers(dpy, (EGLSurface)c->egl_surface) != EGL_TRUE) return false;
  gbm_bo* bo = gbm_surface_lock_front_buffer(c->gbm_surf);
  if (!bo) return false;
  uint32_t handle = gbm_bo_get_handle(bo).u32;
  uint32_t fb_id = 0;
  if (drmModeAddFB(c->fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo), 24, 32,
                   gbm_bo_get_stride(bo), handle, &fb_id) != 0)
    return false;
  if (drmModePageFlip(c->fd, c->crtc_id, fb_id, DRM_MODE_PAGE_FLIP_EVENT,
                      nullptr) != 0) {
    drmModeSetCrtc(c->fd, c->crtc_id, fb_id, 0, 0, &c->connector_id, 1, &c->mode);
  }
  return true;
}
inline bool wait_vblank(void* state, frame_tick* out) {
  return wait_for_vblank(static_cast<kms_ctx*>(state)->fd, out);
}

inline void destroy(void* state) {
  auto* c = static_cast<kms_ctx*>(state);
  if (c->egl_surface) eglDestroySurface((EGLDisplay)c->egl_display, (EGLSurface)c->egl_surface);
  if (c->gbm_surf) gbm_surface_destroy(c->gbm_surf);
  if (c->egl_display) eglTerminate((EGLDisplay)c->egl_display);
  if (c->gbm_dev) gbm_device_destroy(c->gbm_dev);
  if (c->fd >= 0) close(c->fd);
  delete c;
}

inline ::oa::backend::backend* create_kms() {
  auto* b = new ::oa::backend::backend();
  static const backend_api api = {
      .initialize = init,
      .run_loop = run_loop,
      .request_exit = request_exit,
      .egl_surface = egl_surface,
      .egl_display = egl_display,
      .egl_config = egl_config,
      .width = width,
      .height = height,
      .dpi = dpi,
      .flush_framebuffer = flush_framebuffer,
      .wait_for_vblank = wait_vblank,
      .destroy = destroy,
  };
  b->api = &api;
  b->state = new kms_ctx();
  return b;
}

}  // namespace oa::backend::kms
