// oa_kms.h -- DRM/KMS display backend for the OpenAuto embedder.
//
// The backend opens a DRM primary node, configures a connector/CRTC with the
// first available mode, and renders into a GBM-backed EGL surface. Vsync is
// driven by DRM vblank waits and page-flip events.
//
// Copyright 2026 OpenAuto Framework.
#pragma once

#include "oa_backend.h"
#include <xf86drmMode.h>
#include <functional>

struct gbm_device;
struct gbm_surface;
struct gbm_bo;

typedef void* EGLDisplay;
typedef void* EGLConfig;
typedef void* EGLSurface;

namespace oa {

class KmssBackend final : public Backend {
 public:
  KmssBackend() = default;
  ~KmssBackend() override;

  bool Initialize() override;
  void RunLoop(std::function<void(const FrameTick&)> on_vsync,
               std::function<void(const InputEvent&)> on_input) override;
  void RequestExit() override;

  void* egl_surface() override { return (void*)egl_surface_; }
  void* egl_display() override { return (void*)egl_display_; }
  void* egl_config() override { return (void*)egl_config_; }
  uint32_t width() const override { return width_; }
  uint32_t height() const override { return height_; }
  double dpi() const override { return dpi_; }

  bool FlushFramebuffer() override;
  bool WaitForVBlank(FrameTick* tick) override;

  int fd() const { return fd_; }

 private:
  bool OpenDevice();
  bool SetupKms();
  bool SetupEgl();

  int fd_ = -1;
  uint32_t crtc_id_ = 0;
  uint32_t connector_id_ = 0;
  drmModeModeInfo mode_ = {};
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  double dpi_ = 1.0;

  gbm_device* gbm_device_ = nullptr;
  gbm_surface* gbm_surface_ = nullptr;

  EGLDisplay egl_display_ = nullptr;
  EGLConfig egl_config_ = nullptr;
  EGLSurface egl_surface_ = nullptr;

  bool running_ = false;

  std::function<void(const FrameTick&)> on_vsync_;
  std::function<void(const InputEvent&)> on_input_;
};

}  // namespace oa
