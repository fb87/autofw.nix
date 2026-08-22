// main.cpp -- OpenAuto Flutter embedder shell (Wayland / DRM/KMS).
//
// Header-only core: the backend and engine are selected at build time via
// OA_BACKEND and consumed through the functional `backend`/`engine` interfaces.
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

#include "core/oa_backend.h"
#include "core/oa_engine.h"

#ifdef OA_BACKEND_WAYLAND
#include "backends/wayland/oa_wayland.h"
#endif
#ifdef OA_BACKEND_KMS
#include "backends/kms/oa_kms.h"
#endif

namespace {

struct options {
  std::string backend = "auto";
  std::string assets;
  std::string icu;
  bool test_texture = false;  // run a demo external-texture producer
};

void print_usage(const char* prog) {
  std::fprintf(stderr,
               "usage: %s [options]\n"
               "  --assets P         Flutter asset directory (required)\n"
               "  --icu P            icudtl.dat path (default <assets>/icudtl.dat)\n"
               "  --test-texture     run a demo external-texture producer\n"
               "  --help             show this help\n",
               prog);
}

options parse_args(int argc, char** argv) {
  static const option kLong[] = {
      {"backend", required_argument, nullptr, 'b'},
      {"assets", required_argument, nullptr, 'a'},
      {"icu", required_argument, nullptr, 'i'},
      {"test-texture", no_argument, nullptr, 't'},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0},
  };
  options out;
  int c;
  while ((c = getopt_long(argc, argv, "b:a:i:th", kLong, nullptr)) != -1) {
    switch (c) {
      case 'b': out.backend = optarg; break;
      case 'a': out.assets = optarg; break;
      case 'i': out.icu = optarg; break;
      case 't': out.test_texture = true; break;
      case 'h':
      default:
        print_usage(argv[0]);
        std::exit(0);
    }
  }
  return out;
}

constexpr int kDemoTextureId = 1001;

void run_demo_texture_producer(oa::engine::engine* e, bool* running) {
  constexpr int kW = 640, kH = 360;
  std::vector<uint8_t> rgba(static_cast<size_t>(kW) * kH * 4);
  uint64_t frame = 0;
  while (*running) {
    const double t = double(frame % 120) / 120.0;
    for (int y = 0; y < kH; ++y)
      for (int x = 0; x < kW; ++x) {
        auto* p = &rgba[(static_cast<size_t>(y) * kW + x) * 4];
        p[0] = static_cast<uint8_t>((x * 255) / kW);
        p[1] = static_cast<uint8_t>((y * 255) / kH);
        p[2] = static_cast<uint8_t>(255 - (x * 255) / kW);
        p[3] = 255;
        int bx = static_cast<int>(t * kW);
        if (x > bx && x < bx + 40) { p[0] = p[1] = p[2] = 255; }
      }
    oa::engine::push_external_texture_frame(e, kDemoTextureId, kW, kH,
                                            rgba.data());
    ++frame;
    std::this_thread::sleep_for(std::chrono::milliseconds(33));
  }
}

}  // namespace

int main(int argc, char** argv) {
  options opts = parse_args(argc, argv);
  if (opts.assets.empty()) { print_usage(argv[0]); return 1; }
  if (opts.icu.empty()) opts.icu = opts.assets + "/icudtl.dat";

  // --- Resolve and create the backend (fixed at build time via OA_BACKEND). ---
  oa::backend::backend* b = nullptr;
  std::string backend_name = opts.backend;
#ifdef OA_BACKEND_WAYLAND
  if (backend_name == "auto") backend_name = "wayland";
  if (backend_name == "wayland") b = oa::backend::wayland::create_wayland();
  else std::fprintf(stderr, "[oa] built with Wayland; ignoring --backend='%s'\n", backend_name.c_str());
#endif
#ifdef OA_BACKEND_KMS
  if (backend_name == "auto") backend_name = "kms";
  if (backend_name == "kms") b = oa::backend::kms::create_kms();
#endif
  if (!b) { std::fprintf(stderr, "[oa] no backend available\n"); return 1; }

  std::fprintf(stderr, "[oa] backend=%s\n", backend_name.c_str());
  if (!oa::backend::backend_initialize(b)) {
    oa::backend::backend_destroy(b);
    return 1;
  }

  // --- Create the Flutter engine on the platform thread. ----------------------
  oa::engine::engine* e = oa::engine::create(b, opts.assets, opts.icu);
  if (!e) {
    oa::backend::backend_destroy(b);
    return 1;
  }

  // Optional demo external-texture producer.
  std::thread producer;
  bool producer_running = false;
  if (opts.test_texture && oa::engine::register_external_texture(e, kDemoTextureId)) {
    producer_running = true;
    producer = std::thread(run_demo_texture_producer, e, &producer_running);
  }

  // The backend loop blocks here on the platform thread; vsync/input/resize/
  // keymap callbacks feed the engine synchronously.
  oa::backend::backend_run_loop(
      b,
      [e](const oa::frame_tick& t) { oa::engine::on_vsync(e, t); },
      [e](const oa::input_event& ev) { oa::engine::send_input(e, ev); },
      [e](uint32_t w, uint32_t h, double d) { oa::engine::on_resize(e, w, h, d); },
      [e](int fd, uint32_t size) { oa::engine::set_keymap(e, fd, size); });

  // Shutdown in reverse order.
  if (producer.joinable()) {
    producer_running = false;
    producer.join();
    oa::engine::unregister_external_texture(e, kDemoTextureId);
  }
  oa::engine::destroy(e);
  oa::backend::backend_destroy(b);
  return 0;
}
