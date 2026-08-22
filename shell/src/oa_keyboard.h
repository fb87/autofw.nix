// oa_keyboard.h -- Keyboard keymap translation for the embedder.
//
// Flutter's key events need a `physical` (USB HID usage code), a `logical` key
// id, and an optional UTF-8 `character`. Wayland gives us raw Linux evdev
// keycodes and a libxkbcommon keymap fd. This helper translates those into the
// fields Flutter expects.
//
// Copyright 2026 OpenAuto Framework.
#pragma once

#include <cstdint>
#include <string>

struct xkb_context;
struct xkb_keymap;
struct xkb_state;

namespace oa {

struct OaKeyResult {
  bool down = false;
  uint16_t physical_hid = 0;  // Flutter physical key (USB HID usage)
  uint64_t logical = 0;       // Flutter logical key id
  std::string character;      // UTF-8 for printable keys (empty for fun/up)
};

// Translates evdev keycodes via a libxkbcommon keymap.
class Keyboard {
 public:
  Keyboard() = default;
  ~Keyboard();

  // (Re)load the keymap from a Wayland keymap fd (owned by callers' mmap).
  void SetKeymap(int fd, uint32_t size);

  // Translate a single key event. |evdev_keycode| is the raw Linux/wayland
  // keycode (>= 8 per the 1-based xkb convention). Returns fields for Flutter.
  OaKeyResult Translate(uint32_t evdev_keycode, bool pressed);

 private:
  xkb_context* ctx_ = nullptr;
  xkb_keymap* keymap_ = nullptr;
  xkb_state* state_ = nullptr;
};

// Map a Linux evdev keycode (8..255) to a USB HID keyboard usage code.
uint16_t EvdevToHid(uint32_t evdev_keycode);

// Best-effort map of an xkb keysym to Flutter's LogicalKeyboardKey id.
uint64_t MapKeysymLogical(uint32_t keysym);

}  // namespace oa
