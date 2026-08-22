#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xkbcommon/xkbcommon.h>

namespace shell::keyboard
{

struct state
{
    xkb_context* xkb{};
    xkb_keymap* map{};
    xkb_state* xkbd{};
};

namespace detail
{
inline auto evdev_to_hid(std::uint32_t code) -> std::uint16_t
{
    if (code == 1)
        return 0x29;
    if (code >= 59 && code <= 70)
        return 0x3a + (code - 59);
    static const std::uint16_t kDigitRow[13] = {35, 30, 31, 32, 33, 34, 28,
                                                29, 36, 37, 38, 39, 45};
    if (code >= 41 && code <= 53)
        return kDigitRow[code - 41];
    static const std::uint16_t kTopRow[13] = {43, 20, 26, 8,  21, 23, 28,
                                              24, 12, 18, 19, 47, 48};
    if (code >= 15 && code <= 27)
        return kTopRow[code - 15];
    static const std::uint16_t kHomeRow[11] = {57, 4, 22, 7, 9, 10, 11, 13, 14, 51, 40};
    if (code >= 30 && code <= 40)
        return kHomeRow[code - 30];
    static const std::uint16_t kBottomRow[8] = {0xE1, 29, 27, 6, 25, 5, 0xE5, 0xE6};
    if (code >= 44 && code <= 51)
        return kBottomRow[code - 44];
    if (code == 86)
        return 0xE5;
    if (code == 29)
        return 0xE0;
    if (code == 97)
        return 0xE3;
    if (code == 56)
        return 0xE2;
    if (code == 100)
        return 0xE6;
    if (code == 125)
        return 0xE9;
    if (code == 57)
        return 44;
    if (code == 14)
        return 42;
    if (code == 15)
        return 43;
    if (code == 28)
        return 40;
    if (code == 58)
        return 41;
    if (code == 103)
        return 0x52;
    if (code == 108)
        return 0x51;
    if (code == 105)
        return 0x50;
    if (code == 106)
        return 0x4F;
    if (code == 111)
        return 0x4A;
    return static_cast<std::uint16_t>((code << 8) | 0xFF);
}

inline auto keysym_to_logical(std::uint32_t sym) -> std::uint64_t
{
    switch (sym)
    {
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        return 0x0010000000D;
    case XKB_KEY_BackSpace:
        return 0x00100000008;
    case XKB_KEY_Tab:
        return 0x00100000009;
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
} // namespace detail

inline auto has_keymap(const state& s) -> bool
{
    return s.xkbd != nullptr;
}

inline auto set_keymap(state& s, int fd, std::uint32_t size) -> void
{
    if (fd < 0)
        return;
    std::size_t map_size = size ? static_cast<std::size_t>(size) : 4096;
    void* buf = mmap(nullptr, map_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (buf == MAP_FAILED)
    {
        close(fd);
        return;
    }
    if (s.xkb == nullptr)
        s.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (s.map)
        xkb_keymap_unref(s.map);
    if (s.xkbd)
        xkb_state_unref(s.xkbd);
    s.map = xkb_keymap_new_from_buffer(s.xkb, static_cast<const char*>(buf), map_size,
                                       XKB_KEYMAP_FORMAT_TEXT_V1,
                                       XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (s.map)
        s.xkbd = xkb_state_new(s.map);
    munmap(buf, map_size);
    close(fd);
}

inline auto ensure_default_keymap(state& s) -> void
{
    if (s.xkbd && s.map)
        return;
    if (s.xkb == nullptr)
        s.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (s.xkb == nullptr)
        return;
    static const xkb_rule_names kDefaultRules = {.rules = "evdev",
                                                 .model = "pc105",
                                                 .layout = "us",
                                                 .variant = nullptr,
                                                 .options = nullptr};
    xkb_keymap* km =
        xkb_keymap_new_from_names(s.xkb, &kDefaultRules, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (km == nullptr)
        return;
    if (s.map)
        xkb_keymap_unref(s.map);
    if (s.xkbd)
        xkb_state_unref(s.xkbd);
    s.map = km;
    s.xkbd = xkb_state_new(s.map);
}

struct key_result
{
    bool down{};
    std::uint16_t physical_hid{};
    std::uint64_t logical{};
    std::string character;
};

inline auto translate(state& s, std::uint32_t evdev_code, bool pressed) -> key_result
{
    key_result out;
    out.down = pressed;
    out.physical_hid = detail::evdev_to_hid(evdev_code);
    out.logical = 0x00100010000ull | out.physical_hid;
    if (s.xkbd == nullptr || s.map == nullptr)
    {
        ensure_default_keymap(s);
        if (s.xkbd == nullptr || s.map == nullptr)
            return out;
    }
    xkb_keycode_t xkc = static_cast<xkb_keycode_t>(evdev_code) + 8;
    if (xkc >= xkb_keymap_min_keycode(s.map) && xkc <= xkb_keymap_max_keycode(s.map))
    {
        xkb_state_update_key(s.xkbd, xkc, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
        xkb_keysym_t sym = xkb_state_key_get_one_sym(s.xkbd, xkc);
        char buf[16] = {0};
        int n = xkb_state_key_get_utf8(s.xkbd, xkc, buf, sizeof(buf) - 1);
        if (n > 0 && pressed)
            out.character.assign(buf, static_cast<std::size_t>(n));
        if (sym >= 0x20 && sym <= 0x10FFFF && sym != 0x01000000)
            out.logical = sym;
        else
            out.logical = detail::keysym_to_logical(sym);
    }
    return out;
}

} // namespace shell::keyboard
