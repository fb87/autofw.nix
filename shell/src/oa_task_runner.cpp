// oa_task_runner.cpp -- implementation of the Flutter platform task runner.
//
// Copyright 2026 OpenAuto Framework.
#include "oa_task_runner.h"

#include <algorithm>

namespace oa {

namespace {

struct RunnerUserData {
  TaskRunner* runner;
};

bool RunsOnThisThread(void* user_data) {
  return static_cast<RunnerUserData*>(user_data)->runner->IsCurrentThread();
}

void PostTaskCb(FlutterTask task, uint64_t target_time_nanos, void* user_data) {
  static_cast<RunnerUserData*>(user_data)->runner->PostTask(task,
                                                            target_time_nanos);
}

void DestructionCb(void* user_data) {
  delete static_cast<RunnerUserData*>(user_data);
}

}  // namespace

TaskRunner::~TaskRunner() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void TaskRunner::Start() {
  thread_ = std::thread(&TaskRunner::Loop, this);
  thread_id_ = thread_.get_id();
}

void TaskRunner::set_engine(FlutterEngine engine) {
  std::lock_guard<std::mutex> lock(mutex_);
  engine_handle_ = engine;
}

void TaskRunner::RequestStop() {
  std::lock_guard<std::mutex> lock(mutex_);
  running_ = false;
  cv_.notify_all();
}

FlutterTaskRunnerDescription TaskRunner::description() {
  auto* ud = new RunnerUserData();
  ud->runner = this;
  FlutterTaskRunnerDescription desc = {};
  desc.struct_size = sizeof(desc);
  desc.user_data = ud;
  desc.runs_task_on_current_thread_callback = RunsOnThisThread;
  desc.post_task_callback = PostTaskCb;
  desc.identifier = 0;
  desc.destruction_callback = DestructionCb;
  return desc;
}

void TaskRunner::PostTask(FlutterTask task, uint64_t target_time_nanos) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back({task, target_time_nanos});
  }
  cv_.notify_one();
}

void TaskRunner::Loop() {
  while (true) {
    Item item;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (queue_.empty()) {
        if (!running_) break;
        cv_.wait(lock);
        continue;
      }
      auto it = std::min_element(
          queue_.begin(), queue_.end(),
          [](const Item& a, const Item& b) {
            return a.target_time_nanos < b.target_time_nanos;
          });
      item = *it;
      queue_.erase(it);
    }

    uint64_t now = FlutterEngineGetCurrentTime();
    if (item.target_time_nanos > now) {
      std::this_thread::sleep_for(
          std::chrono::nanoseconds(item.target_time_nanos - now));
    }

    FlutterEngine handle;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      handle = engine_handle_;
    }
    if (handle) {
      FlutterEngineRunTask(handle, &item.task);
    }
  }
}

}  // namespace oa
