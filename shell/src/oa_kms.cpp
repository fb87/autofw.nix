// oa_kms.cpp -- DRM/KMS backend implementation.
//
// Copyright 2026 OpenAuto Framework.
#include "oa_kms.h"

#include "oa_egl.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/errno.h>
#include <time.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>

#include <cstdio>
#include <cstring>

namespace oa {

namespace {

// Number of framebuffers in the flip queue. The first one is used to set the
// CRTC; subsequent frames alternate between two GBM surfaces.
constexpr int kFlipPlanes = 2;

}  // namespace

KmssBackend::~KmssBackend() {
  if (egl_surface_) {
    eglDestroySurface(reinterpret_cast<EGLDisplay>(egl_display_),
                      reinterpret_cast<EGLSurface>(egl_surface_));
  }
  if (gbm_surface_) gbm_surface_destroy(gbm_surface_);
  if (egl_display_) eglTerminate(reinterpret_cast<EGLDisplay>(egl_display_));
  if (gbm_device_) gbm_device_destroy(gbm_device_);
  if (fd_ >= 0) close(fd_);
}

bool KmssBackend::Initialize() {
  if (!OpenDevice()) return false;
  if (!SetupKms()) return false;
  if (!SetupEgl()) return false;
  return true;
}

bool KmssBackend::OpenDevice() {
  // Try the primary node, then the render node fallback.
  const char* candidates[] = {
      "/dev/dri/card0", "/dev/dri/card1", "/dev/dri/renderD128", nullptr};
  for (int i = 0; candidates[i]; ++i) {
    int fd = open(candidates[i], O_RDWR | O_CLOEXEC);
    if (fd < 0) continue;
    drmVersionPtr ver = drmGetVersion(fd);
    if (ver) {
      drmFreeVersion(ver);
      fd_ = fd;
      return true;
    }
    close(fd);
  }
  std::fprintf(stderr, "[oa-kms] no usable DRM device found\n");
  return false;
}

bool KmssBackend::SetupKms() {
  drmModeResPtr res = drmModeGetResources(fd_);
  if (!res) {
    std::fprintf(stderr, "[oa-kms] drmModeGetResources failed\n");
    return false;
  }

  // Find the first connected connector and its preferred mode.
  for (int c = 0; c < res->count_connectors; ++c) {
    drmModeConnector* conn = drmModeGetConnector(fd_, res->connectors[c]);
    if (!conn) continue;
    if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
      connector_id_ = conn->connector_id;
      // Prefer the preferred mode / largest.
      drmModeModeInfo* best = &conn->modes[0];
      for (int m = 0; m < conn->count_modes; ++m) {
        if (conn->modes[m].type & DRM_MODE_TYPE_PREFERRED) {
          best = &conn->modes[m];
          break;
        }
      }
      width_ = best->hdisplay;
      height_ = best->vdisplay;
      dpi_ = 1.0;  // physical pixel ratio
      std::memcpy(&mode_, best, sizeof(mode_));

      // Find a CRTC supporting this connector.
      for (int crtc = 0; crtc < res->count_crtcs; ++crtc) {
        if (conn->encoder_id) {
          drmModeEncoderPtr enc = drmModeGetEncoder(fd_, conn->encoder_id);
          if (enc && (enc->possible_crtcs & (1 << crtc))) {
            crtc_id_ = res->crtcs[crtc];
          }
          if (enc) drmModeFreeEncoder(enc);
        }
      }
      drmModeFreeConnector(conn);
      break;
    }
    drmModeFreeConnector(conn);
  }
  drmModeFreeResources(res);

  if (!connector_id_ || !crtc_id_) {
    std::fprintf(stderr,
                 "[oa-kms] no suitable connector/CRTC configuration found\n");
    return false;
  }
  return true;
}

bool KmssBackend::SetupEgl() {
  gbm_device_ = gbm_create_device(fd_);
  if (!gbm_device_) {
    std::fprintf(stderr, "[oa-kms] gbm_create_device failed\n");
    return false;
  }

  gbm_surface_ = gbm_surface_create(gbm_device_, width_, height_,
                                    GBM_FORMAT_XRGB8888,
                                    GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
  if (!gbm_surface_) {
    std::fprintf(stderr, "[oa-kms] gbm_surface_create failed\n");
    return false;
  }

  static const EGLAttrib kAttribs[] = {EGL_NONE};
  egl_display_ = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, (void*)gbm_device_,
                                       kAttribs);
  if (!egl_display_) {
    std::fprintf(stderr, "[oa-kms] eglGetPlatformDisplay failed\n");
    return false;
  }
  if (eglInitialize(reinterpret_cast<EGLDisplay>(egl_display_), nullptr,
                    nullptr) != EGL_TRUE) {
    std::fprintf(stderr, "[oa-kms] eglInitialize failed\n");
    return false;
  }
  if (!egl::ChooseConfig(reinterpret_cast<EGLDisplay>(egl_display_),
                         reinterpret_cast<EGLConfig*>(&egl_config_))) {
    return false;
  }
  egl_surface_ = eglCreatePlatformWindowSurface(
      reinterpret_cast<EGLDisplay>(egl_display_),
      reinterpret_cast<EGLConfig>(egl_config_), (void*)gbm_surface_, nullptr);
  if (egl_surface_ == EGL_NO_SURFACE) {
    std::fprintf(stderr, "[oa-kms] eglCreatePlatformWindowSurface failed\n");
    return false;
  }
  return true;
}

void KmssBackend::RunLoop(std::function<void(const FrameTick&)> on_vsync,
                          std::function<void(const InputEvent&)> on_input) {
  on_vsync_ = std::move(on_vsync);
  on_input_ = std::move(on_input);
  running_ = true;

  while (running_) {
    // Wait for vblank so we present at the refresh boundary.
    FrameTick tick;
    if (!WaitForVBlank(&tick)) break;
    if (on_vsync_) on_vsync_(tick);
  }
  running_ = false;
}

void KmssBackend::RequestExit() { running_ = false; }

bool KmssBackend::WaitForVBlank(FrameTick* tick) {
  drmVBlank vblank = {};
  vblank.request.type = DRM_VBLANK_RELATIVE;
  vblank.request.sequence = 1;
  if (drmWaitVBlank(fd_, &vblank) < 0) {
    return false;
  }
  if (tick) {
    struct timespec ts;
    uint64_t ns;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
      ns = (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
    } else {
      ns = 0;
    }
    // Assume 60Hz pacing; a precise value would come from the chosen mode.
    tick->present_ns = ns;
    tick->target_ns = ns + 16666667ull;  // ~60 frames per second
  }
  return true;
}

bool KmssBackend::FlushFramebuffer() {
  EGLDisplay dpy = reinterpret_cast<EGLDisplay>(egl_display_);
  // The engine's `make_current` callback has already bound our context to
  // this surface, so we only need to present it.
  if (eglSwapBuffers(dpy, reinterpret_cast<EGLSurface>(egl_surface_)) !=
      EGL_TRUE) {
    return false;
  }

  // Turn the front GBM buffer into a DRM framebuffer and flip it on-screen.
  gbm_bo* bo = gbm_surface_lock_front_buffer(gbm_surface_);
  if (!bo) return false;
  uint32_t handle = gbm_bo_get_handle(bo).u32;
  uint32_t fb_id = 0;
  if (drmModeAddFB(fd_, gbm_bo_get_width(bo), gbm_bo_get_height(bo), 24, 32,
                   gbm_bo_get_stride(bo), handle, &fb_id) != 0) {
    std::fprintf(stderr, "[oa-kms] drmModeAddFB failed\n");
    return false;
  }
  int flip_rc = drmModePageFlip(fd_, crtc_id_, fb_id,
                                DRM_MODE_PAGE_FLIP_EVENT, nullptr);
  if (flip_rc != 0) {
    // Fall back to a blocking setcrtc if page flip not possible.
    drmModeSetCrtc(fd_, crtc_id_, fb_id, 0, 0, &connector_id_, 1, &mode_);
  }
  // A production shell would keep a small flip queue and release a front
  // buffer only after the matching DRM_EVENT_FLIP_COMPLETE arrives.
  return true;
}

Backend* CreateKmsBackend() { return new KmssBackend(); }

}  // namespace oa
