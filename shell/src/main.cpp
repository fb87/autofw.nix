// main.cpp -- OpenAuto Flutter embedder shell.
//
// A minimal embedder-from-scratch Flutter host that renders to a Wayland
// compositor or to a DRM/KMS display. The active platform is chosen on the
// command line or by probing the environment.
//
// Usage:
//   openauto --backend=wayland|kms --assets=/path/to/assets [--icu=icudtl.dat]
//
// Copyright 2026 OpenAuto Framework.
#include <getopt.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "oa_backend.h"
#include "oa_engine.h"

namespace {

struct Options {
  std::string backend = "auto";
  std::string assets;
  std::string icu;
  bool test_texture = false;  // run a demo external-texture producer
};

void PrintUsage(const char* prog) {
  std::fprintf(stderr,
               "usage: %s [options]\n"
               "  --backend B        rendering backend: wayland | kms (default auto)\n"
               "  --assets P         Flutter asset directory (required)\n"
               "  --icu P            icudtl.dat path (default <assets>/icudtl.dat)\n"
               "  --test-texture     run a demo external-texture producer\n"
               "  --help             show this help\n",
               prog);
}

Options ParseArgs(int argc, char** argv) {
  static const struct option kLongOpts[] = {
      {"backend", required_argument, nullptr, 'b'},
      {"assets", required_argument, nullptr, 'a'},
      {"icu", required_argument, nullptr, 'i'},
      {"test-texture", no_argument, nullptr, 't'},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0},
  };
  Options out;
  int c;
  while ((c = getopt_long(argc, argv, "b:a:i:th", kLongOpts, nullptr)) != -1) {
    switch (c) {
      case 'b': out.backend = optarg; break;
      case 'a': out.assets = optarg; break;
      case 'i': out.icu = optarg; break;
      case 't': out.test_texture = true; break;
      case 'h':
      default:
        PrintUsage(argv[0]);
        std::exit(0);
    }
  }
  return out;
}

// Demo external-texture producer. Registers texture id |kDemoTextureId| and
// pushes a continuously animated RGBA frame to it from a dedicated thread at
// ~30 fps, standing in for a real video sink (Android Auto / GStreamer).
constexpr const int kDemoTextureId = 1001;

void RunDemoTextureProducer(oa::Engine* engine, bool* running) {
  constexpr int kW = 640, kH = 360;
  std::vector<uint8_t> rgba(static_cast<size_t>(kW) * kH * 4);
  uint64_t frame = 0;
  while (*running) {
    // Paint a simple animated pattern: gradient background + moving bar.
    const double t = double(frame % 120) / 120.0;
    for (int y = 0; y < kH; ++y) {
      for (int x = 0; x < kW; ++x) {
        auto* p = &rgba[(static_cast<size_t>(y) * kW + x) * 4];
        const uint8_t r = static_cast<uint8_t>((x * 255) / kW);
        const uint8_t g = static_cast<uint8_t>((y * 255) / kH);
        const uint8_t b = static_cast<uint8_t>(255 - (x * 255) / kW);
        p[0] = r;
        p[1] = g;
        p[2] = b;
        p[3] = 255;
        // Moving vertical bar.
        const int bx = static_cast<int>(t * kW);
        if (x > bx && x < bx + 40) {
          p[0] = 255;
          p[1] = 255;
          p[2] = 255;
        }
      }
    }
    engine->PushExternalTextureFrame(kDemoTextureId, kW, kH, rgba.data());
    ++frame;
    std::this_thread::sleep_for(std::chrono::milliseconds(33));  // ~30 fps
  }
}

}  // namespace

int main(int argc, char** argv) {
  Options opts = ParseArgs(argc, argv);
  if (opts.assets.empty()) {
    PrintUsage(argv[0]);
    return 1;
  }
  if (opts.icu.empty()) opts.icu = opts.assets + "/icudtl.dat";

  // --- Resolve the render backend. -------------------------------------------
  oa::Backend* backend = nullptr;
  std::string backend_name = opts.backend;
  if (backend_name == "auto") {
    backend_name = ::getenv("WAYLAND_DISPLAY") ? "wayland" : "kms";
  }
  if (backend_name == "wayland") {
    backend = oa::CreateWaylandBackend();
  } else if (backend_name == "kms") {
#ifdef OA_HAVE_KMS
    backend = oa::CreateKmsBackend();
#else
    std::fprintf(stderr, "[oa] built without KMS backend support\n");
    return 1;
#endif
  } else {
    std::fprintf(stderr, "[oa] unknown backend '%s'\n", backend_name.c_str());
    return 1;
  }

  std::fprintf(stderr, "[oa] backend=%s\n", backend_name.c_str());
  if (!backend->Initialize()) {
    std::fprintf(stderr, "[oa] backend initialization failed\n");
    delete backend;
    return 1;
  }

  // --- Start the Flutter engine on this (platform) thread. ---------------------
  std::unique_ptr<oa::Engine> engine =
      oa::Engine::Create(backend, opts.assets, opts.icu);
  if (!engine) {
    std::fprintf(stderr, "[oa] failed to start Flutter engine\n");
    delete backend;
    return 1;
  }

  // Optional demo external-texture producer: registers texture id and pushes
  // animated frames while the app runs (e.g. a gallery Texture widget).
  std::thread producer;
  bool producer_running = false;
  if (opts.test_texture) {
    if (engine->RegisterExternalTexture(kDemoTextureId)) {
      producer_running = true;
      producer = std::thread(RunDemoTextureProducer, engine.get(),
                             &producer_running);
      std::fprintf(stderr,
                   "[oa] demo external texture #%d producer started\n",
                   kDemoTextureId);
    }
  }

  // The backend event loop blocks here on the platform thread. Vsync and input
  // callbacks are delivered synchronously, so FlutterEngineOnVsync and the
  // FlutterEngineSend* input calls all run on the thread that called
  // FlutterEngineRun, as the embedder requires. Flutter does its own
  // rendering on internal render threads.
  backend->RunLoop(
      [&engine](const oa::FrameTick& tick) { engine->OnVsync(tick); },
      [&engine](const oa::InputEvent& ev) { engine->SendInputEvent(ev); });

  // Shutdown in reverse order of construction.
  if (producer.joinable()) {
    producer_running = false;
    producer.join();
    engine->UnregisterExternalTexture(kDemoTextureId);
  }
  engine.reset();
  delete backend;
  return 0;
}
