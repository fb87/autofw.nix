// oa_keyboard.h -- Keyboard keymap translation. Header-only.
//
// Namespace-based functional module (no classes). Flutter key events need a
// `physical` (USB HID usage), `logical` key id, and a UTF-8 `character`.
// Wayland gives raw Linux evdev keycodes and (usually) a libxkbcommon keymap
// fd; this translates them, falling back to a default US keymap when the
// compositor sends none.
//
// Copyright 2026 OpenAuto Framework.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xkbcommon/xkbcommon.h>

#include "oa_types.h"

namespace oa::keyboard {

// Opaque state for the keymap context.
struct ctx {
  xkb_context* xkb = nullptr;
  xkb_keymap* keymap = nullptr;
  xkb_state* state = nullptr;
};

// Create / destroy a keyboard context.
inline void init(ctx* c) {
  c->xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
}
inline void destroy(ctx* c) {
  if (c->state) xkb_state_unref(c->state);
  if (c->keymap) xkb_keymap_unref(c->keymap);
  if (c->xkb) xkb_context_unref(c->xkb);
  c->xkb = nullptr;
  c->keymap = nullptr;
  c->state = nullptr;
}
inline bool has_keymap(const ctx* c) { return c->state != nullptr; }

// (Re)load the keymap from a Wayland keymap fd (the call takes ownership and
// closes the fd).
inline void set_keymap(ctx* c, int fd, uint32_t size) {
  if (fd < 0) return;
  size_t map_size = size ? static_cast<size_t>(size) : 4096;
  void* buf = mmap(nullptr, map_size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (buf == MAP_FAILED) {
    close(fd);
    return;
  }
  if (!c->xkb) c->xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (c->keymap) xkb_keymap_unref(c->keymap);
  if (c->state) xkb_state_unref(c->state);
  c->keymap = xkb_keymap_new_from_buffer(
      c->xkb, static_cast<const char*>(buf), map_size,
      XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
  if (c->keymap) c->state = xkb_state_new(c->keymap);
  munmap(buf, map_size);
  close(fd);
}

// If the compositor never provided a keymap, build a default (US) one so text
// input works even against compositors that omit wl_keyboard.keymap.
inline void ensure_default_keymap(ctx* c) {
  if (c->state && c->keymap) return;
  if (!c->xkb) c->xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (!c->xkb) return;
  static const xkb_rule_names kDefaultRules = {
      .rules = "evdev", .model = "pc105", .layout = "us", .variant = nullptr,
      .options = nullptr};
  xkb_keymap* km =
      xkb_keymap_new_from_names(c->xkb, &kDefaultRules, XKB_KEYMAP_COMPILE_NO_FLAGS);
  if (!km) return;
  if (c->keymap) xkb_keymap_unref(c->keymap);
  if (c->state) xkb_state_unref(c->state);
  c->keymap = km;
  c->state = xkb_state_new(c->keymap);
}

// Map a Linux evdev keycode to a USB HID keyboard usage code.
inline uint16_t evdev_to_hid(uint32_t code) {
  if (code == 1) return 0x29;
  if (code >= 59 && code <= 70) return 0x3a + (code - 59);
  static const uint16_t kDigitRow[13] = {35, 30, 31, 32, 33, 34, 28,
                                         29, 36, 37, 38, 39, 45};
  if (code >= 41 && code <= 53) return kDigitRow[code - 41];
  static const uint16_t kTopRow[13] = {43, 20, 26, 8, 21, 23, 28, 24,
                                       12, 18, 19, 47, 48};
  if (code >= 15 && code <= 27) return kTopRow[code - 15];
  static const uint16_t kHomeRow[11] = {57, 4, 22, 7, 9, 10, 11, 13, 14, 51, 40};
  if (code >= 30 && code <= 40) return kHomeRow[code - 30];
  static const uint16_t kBottomRow[8] = {0xE1, 29, 27, 6, 25, 5, 0xE5, 0xE6};
  if (code >= 44 && code <= 51) return kBottomRow[code - 44];
  if (code == 86) return 0xE5;
  if (code == 29) return 0xE0;
  if (code == 97) return 0xE3;
  if (code == 56) return 0xE2;
  if (code == 100) return 0xE6;
  if (code == 125) return 0xE9;
  if (code == 57) return 44;
  if (code == 42 || code == 54) return 0xE1;
  if (code == 14) return 42;
  if (code == 15) return 43;
  if (code == 28) return 40;
  if (code == 58) return 41;
  if (code == 103) return 0x52;
  if (code == 108) return 0x51;
  if (code == 105) return 0x50;
  if (code == 106) return 0x4F;
  if (code == 111) return 0x4A;
  return (code << 8) | 0xFF;
}

// Best-effort map of an xkb keysym to Flutter's LogicalKeyboardKey id.
inline uint64_t keysym_to_logical(uint32_t sym) {
  switch (sym) {
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter: return 0x0010000000D;
    case XKB_KEY_BackSpace: return 0x00100000008;
    case XKB_KEY_Tab: return 0x00100000009;
    case XKB_KEY_Escape: return 0x0010000001B;
    case XKB_KEY_Delete: return 0x0010000007F;
    case XKB_KEY_Left: return 0x00100000050;
    case XKB_KEY_Up: return 0x00100000052;
    case XKB_KEY_Right: return 0x0010000004F;
    case XKB_KEY_Down: return 0x00100000051;
    case XKB_KEY_Home: return 0x00100000050;
    case XKB_KEY_End: return 0x00100000047;
    default: return 0x00100000000 | (sym & 0xFFFF);
  }
}

// Translate one key event. Fills Flutter key-event fields.
inline key_result translate(ctx* c, uint32_t evdev_keycode, bool pressed) {
  key_result out;
  out.down = pressed;
  out.physical_hid = evdev_to_hid(evdev_keycode);
  out.logical = 0x00100010000ull | out.physical_hid;  // never 0 (Flutter rejects 0)

  if (!c->state || !c->keymap) {
    ensure_default_keymap(c);
    if (!c->state || !c->keymap) return out;
  }

  const xkb_keycode_t xkc = static_cast<xkb_keycode_t>(evdev_keycode) + 8;
  if (xkc >= xkb_keymap_min_keycode(c->keymap) &&
      xkc <= xkb_keymap_max_keycode(c->keymap)) {
    xkb_state_update_key(c->state, xkc, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
    xkb_keysym_t sym = xkb_state_key_get_one_sym(c->state, xkc);
    char buf[16] = {0};
    int n = xkb_state_key_get_utf8(c->state, xkc, buf, sizeof(buf) - 1);
    if (n > 0 && pressed) out.character.assign(buf, static_cast<size_t>(n));
    if (sym >= 0x20 && sym <= 0x10FFFF && sym != 0x01000000) {
      out.logical = sym;  // printable key
    } else {
      out.logical = keysym_to_logical(sym);
    }
  }
  return out;
}

}  // namespace oa::keyboard
