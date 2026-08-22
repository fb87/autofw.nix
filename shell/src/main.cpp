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

#include <cstdio>
#include <cstdlib>
#include <string>

#include "oa_backend.h"
#include "oa_engine.h"

namespace {

struct Options {
  std::string backend = "auto";
  std::string assets;
  std::string icu;
};

void PrintUsage(const char* prog) {
  std::fprintf(stderr,
               "usage: %s [options]\n"
               "  --backend B  rendering backend: wayland | kms (default auto)\n"
               "  --assets P   Flutter asset directory (required)\n"
               "  --icu P      icudtl.dat path (default <assets>/icudtl.dat)\n"
               "  --help       show this help\n",
               prog);
}

Options ParseArgs(int argc, char** argv) {
  static const struct option kLongOpts[] = {
      {"backend", required_argument, nullptr, 'b'},
      {"assets", required_argument, nullptr, 'a'},
      {"icu", required_argument, nullptr, 'i'},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0},
  };
  Options out;
  int c;
  while ((c = getopt_long(argc, argv, "b:a:i:h", kLongOpts, nullptr)) != -1) {
    switch (c) {
      case 'b': out.backend = optarg; break;
      case 'a': out.assets = optarg; break;
      case 'i': out.icu = optarg; break;
      case 'h':
      default:
        PrintUsage(argv[0]);
        std::exit(0);
    }
  }
  return out;
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

  // The backend event loop blocks here on the platform thread. Vsync and input
  // callbacks are delivered synchronously, so FlutterEngineOnVsync and the
  // FlutterEngineSend* input calls all run on the thread that called
  // FlutterEngineRun, as the embedder requires. Flutter does its own
  // rendering on internal render threads.
  backend->RunLoop(
      [&engine](const oa::FrameTick& tick) { engine->OnVsync(tick); },
      [&engine](const oa::InputEvent& ev) { engine->SendInputEvent(ev); });

  // Shutdown in reverse order of construction.
  engine.reset();
  delete backend;
  return 0;
}
