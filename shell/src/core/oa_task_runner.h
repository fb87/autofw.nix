// oa_task_runner.h -- Platform task runner. Header-only.
//
// Namespace-based functional module (no classes). Flutter delivers async work
// (most importantly platform-message callbacks and key events) on the platform
// task runner. Because the platform thread is consumed by the display loop, we
// give Flutter a dedicated runner serviced by its own thread.
//
// Copyright 2026 OpenAuto Framework.
#pragma once

#include "flutter_embedder.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace oa::task_runner {

struct item {
  std::function<void()> fn;      // generic closure (if set)
  FlutterTask task;
  uint64_t target_time_nanos = 0;
  bool is_closure = false;
};

// State for a platform task runner + its servicing thread.
struct ctx {
  FlutterEngine engine_handle = nullptr;
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<item> queue;
  bool running = true;
  std::thread thread;
  std::thread::id thread_id;
  bool started = false;
};

namespace detail {
struct user_data {
  ctx* c;
};
}  // namespace detail

inline bool runs_on_this_thread(void* ud) {
  auto* d = static_cast<detail::user_data*>(ud);
  return std::this_thread::get_id() == d->c->thread_id;
}
inline void post_task_cb(FlutterTask task, uint64_t target_time_nanos,
                         void* ud) {
  auto* d = static_cast<detail::user_data*>(ud);
  {
    std::lock_guard<std::mutex> lock(d->c->mutex);
    d->c->queue.push_back(item{nullptr, task, target_time_nanos, false});
  }
  d->c->cv.notify_one();
}
inline void destruction_cb(void* ud) { delete static_cast<detail::user_data*>(ud); }

// The FlutterTaskRunnerDescription to pass to the engine.
inline FlutterTaskRunnerDescription description(ctx* c) {
  detail::user_data* ud = new detail::user_data{};
  ud->c = c;
  FlutterTaskRunnerDescription desc = {};
  desc.struct_size = sizeof(desc);
  desc.user_data = ud;
  desc.runs_task_on_current_thread_callback = runs_on_this_thread;
  desc.post_task_callback = post_task_cb;
  desc.identifier = 0;
  desc.destruction_callback = destruction_cb;
  return desc;
}

inline void start(ctx* c) {
  if (c->started) return;
  c->started = true;
  c->thread = std::thread([c]() {
    c->thread_id = std::this_thread::get_id();
    while (true) {
      item it;
      {
        std::unique_lock<std::mutex> lock(c->mutex);
        if (c->queue.empty()) {
          if (!c->running) break;
          c->cv.wait(lock);
          continue;
        }
        auto itr = std::min_element(
            c->queue.begin(), c->queue.end(),
            [](const item& a, const item& b) {
              if (a.is_closure != b.is_closure) return a.is_closure;
              return a.target_time_nanos < b.target_time_nanos;
            });
        it = *itr;
        c->queue.erase(itr);
      }
      if (it.is_closure) {
        if (it.fn) it.fn();
        continue;
      }
      uint64_t now = FlutterEngineGetCurrentTime();
      if (it.target_time_nanos > now) {
        std::this_thread::sleep_for(
            std::chrono::nanoseconds(it.target_time_nanos - now));
      }
      FlutterEngine handle;
      {
        std::lock_guard<std::mutex> lock(c->mutex);
        handle = c->engine_handle;
      }
      if (handle) FlutterEngineRunTask(handle, &it.task);
    }
  });
}

inline void stop(ctx* c) {
  {
    std::lock_guard<std::mutex> lock(c->mutex);
    c->running = false;
  }
  c->cv.notify_all();
  if (c->thread.joinable()) c->thread.join();
  c->started = false;
}

inline void set_engine(ctx* c, FlutterEngine engine) {
  std::lock_guard<std::mutex> lock(c->mutex);
  c->engine_handle = engine;
}

inline bool is_current_thread(const ctx* c) {
  return std::this_thread::get_id() == c->thread_id;
}

// Run a generic closure on the task runner's thread (the platform thread).
inline void post_closure(ctx* c, std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lock(c->mutex);
    c->queue.push_back(item{std::move(fn), FlutterTask{}, 0, true});
  }
  c->cv.notify_one();
}

}  // namespace oa::task_runner
