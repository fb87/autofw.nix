// oa_types.h -- Plain data types shared by the OpenAuto embedder.
//
// Header-only. Functional, snake_case — there are no classes here, just data
// structs used by the namespace-based modules.
//
// Copyright 2026 OpenAuto Framework.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace oa {

// One (or zero) vsync/frame-tick outstanding. The backend fills in the target
// frame time so the engine can schedule its frame work.
struct frame_tick {
  // ns (monotonic) when the frame is presented. Used as frame_target_time.
  uint64_t target_ns = 0;
  // ns (monotonic) when the frame started, before the refresh interval.
  uint64_t present_ns = 0;
};

// A flattened input event delivered to the render loop.
enum class input_device_kind { mouse, touch, stylus, trackpad, keyboard };

struct input_event {
  input_device_kind device{input_device_kind::mouse};
  // Wayland-style button mask (bits for primary/secondary/middle).
  uint32_t buttons = 0;
  int32_t slot = -1;
  double x = 0.0;   // physical px, window-relative
  double y = 0.0;
  double scroll_x = 0.0;
  double scroll_y = 0.0;
  // Keyboard.
  bool key_down = false;
  bool key_repeat = false;
  uint32_t keycode = 0;  // Linux evdev keycode
  uint32_t keysym = 0;   // optional, else 0
  // Pointer lifecycle flags.
  bool add = false;     // cursor entered the view (kAdd)
  bool remove = false;  // cursor left the view (kRemove)
};

// Result of a keyboard key translation, used to build a Flutter key event.
struct key_result {
  bool down = false;
  uint16_t physical_hid = 0;  // Flutter physical key (USB HID usage)
  uint64_t logical = 0;       // Flutter logical key id
  std::string character;      // UTF-8 for printable keys (empty for up keys)
};

// Information about one external texture the engine needs to sample it.
struct external_texture_info {
  uint32_t gl_name = 0;      // GL texture name
  uint32_t gl_target = 0x0DE1;  // GL_TEXTURE_2D
  uint32_t gl_format = 0x8058;  // GL_RGBA8
  uint32_t width = 0;
  uint32_t height = 0;
  bool ready = false;
};

}  // namespace oa
