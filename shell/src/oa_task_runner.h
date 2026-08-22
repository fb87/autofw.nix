// oa_task_runner.h -- An embedder-owned Flutter task runner for the platform
// thread.
//
// Flutter delivers asynchronous work (most importantly the platform message
// callback) on the platform task runner. Because our platform thread is
// consumed by the display backend event loop, we hand Flutter a dedicated task
// runner serviced by its own thread, using the recommended
// FlutterEngineInitialize + FlutterEngineRunInitialized sequence.
//
// Copyright 2026 OpenAuto Framework.
#pragma once

#include "flutter_embedder.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace oa {

class TaskRunner {
 public:
  TaskRunner() = default;
  ~TaskRunner();

  FlutterTaskRunnerDescription description();
  void Start();
  void RequestStop();
  void set_engine(FlutterEngine engine);

  void PostTask(FlutterTask task, uint64_t target_time_nanos);
  // Run an arbitrary closure on the task runner's thread (the platform thread
  // Flutter associates with this runner). Used to marshal native API calls
  // (e.g. key events) that Flutter requires on the platform thread.
  void PostClosure(std::function<void()> fn);
  bool IsCurrentThread() const {
    return std::this_thread::get_id() == thread_id_;
  }

 private:
  void Loop();

  FlutterEngine engine_handle_ = nullptr;
  std::mutex mutex_;
  std::condition_variable cv_;
  struct Item {
    std::function<void()> fn;      // generic closure (if set)
    FlutterTask task;
    uint64_t target_time_nanos;
    bool is_closure = false;
  };
  std::deque<Item> queue_;
  bool running_ = true;
  std::thread thread_;
  std::thread::id thread_id_;
};

}  // namespace oa
