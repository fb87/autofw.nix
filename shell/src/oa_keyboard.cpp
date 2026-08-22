// oa_keyboard.cpp -- keyboard keymap translation.
//
// Copyright 2026 OpenAuto Framework.
#include "oa_keyboard.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstring>

#include <xkbcommon/xkbcommon.h>

namespace oa {

Keyboard::~Keyboard() {
  if (state_) xkb_state_unref(state_);
  if (keymap_) xkb_keymap_unref(keymap_);
  if (ctx_) xkb_context_unref(ctx_);
}

void Keyboard::SetKeymap(int fd, uint32_t size) {
  if (fd < 0) return;
  size_t map_size = size ? static_cast<size_t>(size) : 4096;
  void* buf = mmap(nullptr, map_size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (buf == MAP_FAILED) {
    close(fd);
    return;
  }
  if (!ctx_) ctx_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (keymap_) xkb_keymap_unref(keymap_);
  if (state_) xkb_state_unref(state_);
  keymap_ = xkb_keymap_new_from_buffer(
      ctx_, static_cast<const char*>(buf), map_size, XKB_KEYMAP_FORMAT_TEXT_V1,
      XKB_KEYMAP_COMPILE_NO_FLAGS);
  if (keymap_) state_ = xkb_state_new(keymap_);
  munmap(buf, map_size);
  close(fd);
}

void Keyboard::EnsureDefaultKeymap() {
  if (state_ && keymap_) return;
  if (!ctx_) ctx_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (!ctx_) return;
  static const xkb_rule_names kDefaultRules = {
      .rules = "evdev", .model = "pc105", .layout = "us", .variant = nullptr,
      .options = nullptr};
  xkb_keymap* km = xkb_keymap_new_from_names(
      ctx_, &kDefaultRules, XKB_KEYMAP_COMPILE_NO_FLAGS);
  if (!km) {
    std::fprintf(stderr, "[oa-keyboard] failed to build default keymap\n");
    return;
  }
  if (keymap_) xkb_keymap_unref(keymap_);
  if (state_) xkb_state_unref(state_);
  keymap_ = km;
  state_ = xkb_state_new(keymap_);
  std::fprintf(stderr,
               "[oa-keyboard] using default US keymap (compositor sent none)\n");
}

OaKeyResult Keyboard::Translate(uint32_t evdev_keycode, bool pressed) {
  OaKeyResult out;
  out.down = pressed;
  out.physical_hid = EvdevToHid(evdev_keycode);
  // Flutter rejects key events with physical==0 or logical==0, so always
  // provide a stable non-zero fallback derived from the HID code.
  out.logical = 0x00100010000ull | out.physical_hid;

  // If the compositor never provided a keymap (e.g. via a virtual keyboard
  // path that omits wl_keyboard.keymap), fall back to a default so character
  // resolution works.
  if (!state_ || !keymap_) {
    EnsureDefaultKeymap();
    if (!state_ || !keymap_) {
      return out;
    }
  }

  // xkb uses 8 + evdev code as its keycode.
  const xkb_keycode_t xkc = static_cast<xkb_keycode_t>(evdev_keycode) + 8;
  if (xkc >= xkb_keymap_min_keycode(keymap_) &&
      xkc <= xkb_keymap_max_keycode(keymap_)) {
    xkb_state_update_key(state_, xkc, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
    xkb_keysym_t sym = xkb_state_key_get_one_sym(state_, xkc);

    // Character (UTF-8) for printable keys.
    char buf[16] = {0};
    int n = xkb_state_key_get_utf8(state_, xkc, buf, sizeof(buf) - 1);
    if (n > 0 && pressed) out.character.assign(buf, static_cast<size_t>(n));

    // logical key id. Flutter treats printable Unicode as the codepoint.
    if (sym >= 0x20 && sym <= 0x10FFFF && sym != 0x01000000) {
      out.logical = sym;  // printable key (including ASCII)
    } else {
      out.logical = MapKeysymLogical(sym);
    }
  }
  return out;
}

// Maps an xkb keysym to Flutter's LogicalKeyboardKey id for common
// non-printable keys. Returned ids follow Flutter's LogicalKeyboardKey layout
// (high bits + column/row). Unmapped keysyms get a stable, unique value.
uint16_t EvdevToHid(uint32_t code) {
  // Row 1 (Escape, F1..F12). evdev 1:Esc, 59..70:F1..F12.
  if (code == 1) return 0x29;          // HID Esc
  if (code >= 59 && code <= 70) return 0x3a + (code - 59);  // F1..F12
  // Row 2: Backquote(41), 1..=, and punctuation.
  static const uint16_t kDigitRow[13] = {35, 30, 31, 32, 33, 34, 28,
                                         29, 36, 37, 38, 39, 45};  // ` 1 2 3 4 5 6 7 8 9 0 - =
  if (code >= 41 && code <= 53) return kDigitRow[code - 41];
  // Row 3: Tab, Q..], (15..27)
  static const uint16_t kTopRow[13] = {43, 20, 26, 8, 21, 23, 28, 24,
                                       12, 18, 19, 47, 48};  // Tab Q W E R T Y U I O P [ ]
  if (code >= 15 && code <= 27) return kTopRow[code - 15];
  // Row 4: Caps, A..', Enter (30..40)
  static const uint16_t kHomeRow[11] = {57, 4, 22, 7, 9, 10, 11, 13, 14, 51, 40};
  if (code >= 30 && code <= 40) return kHomeRow[code - 30];
  // Row 5: Shift, Z../, right Shift, Ctrl(29), Win(125), Alt(56), space(57)
  static const uint16_t kBottomRow[8] = {0xE1, 29, 27, 6, 25, 5, 0xE5, 0xE6};  // Lshift Z X C V B N M
  if (code >= 44 && code <= 51) return kBottomRow[code - 44];
  if (code == 86) return 0xE5;  // Rshift
  if (code == 29) return 0xE0;  // Lctrl
  if (code == 97) return 0xE3;  // Rctrl
  if (code == 56) return 0xE2;  // Lalt
  if (code == 100) return 0xE6; // Ralt
  if (code == 125) return 0xE3 + 6; // Meta (approx)
  if (code == 57) return 44;    // Space (HID 0x2C)
  if (code == 42 || code == 54) return 0xE1;  // Shift
  if (code == 14) return 42;    // Backspace
  if (code == 15) return 43;    // Tab
  if (code == 28) return 40;    // Enter
  if (code == 58) return 41;    // Caps
  if (code == 103) return 0x52; // Up
  if (code == 108) return 0x51; // Down
  if (code == 105) return 0x50; // Left
  if (code == 106) return 0x4F; // Right
  if (code == 111) return 0x4A; // Delete
  return (code << 8) | 0xFF;    // stable fallback
}

uint64_t MapKeysymLogical(xkb_keysym_t sym) {
  // Best-effort Flutter LogicalKeyboardKey ids for common keys.
  switch (sym) {
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
      return 0x0010000000D;  // Flutter keyEnter
    case XKB_KEY_BackSpace:
      return 0x00100000008;  // keyBackspace
    case XKB_KEY_Tab:
      return 0x00100000009;  // keyTab
    case XKB_KEY_Escape:
      return 0x0010000001B;
    case XKB_KEY_Delete:
      return 0x0010000007F;
    case XKB_KEY_Left:
      return 0x00100000050;
    case XKB_KEY_Up:
      return 0x00100000052;
    case XKB_KEY_Right:
      return 0x0010000004F;
    case XKB_KEY_Down:
      return 0x00100000051;
    case XKB_KEY_Home:
      return 0x00100000050;
    case XKB_KEY_End:
      return 0x00100000047;
    default:
      return 0x00100000000 | (sym & 0xFFFF);
  }
}
}  // namespace oa
