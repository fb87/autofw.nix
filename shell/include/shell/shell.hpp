#pragma once

#include <shell/clipboard.hpp>
#include <shell/config.hpp>
#include <shell/egl.hpp>
#include <shell/engine.hpp>
#include <shell/error.hpp>
#include <shell/event.hpp>
#include <shell/handle.hpp>
#include <shell/input.hpp>
#include <shell/keyboard.hpp>
#include <shell/layer.hpp>
#include <shell/log.hpp>
#include <shell/native.hpp>
#include <shell/plugin.hpp>
#include <shell/renderer.hpp>
#include <shell/result.hpp>
#include <shell/selection.hpp>
#include <shell/shared.hpp>
#include <shell/software.hpp>
#include <shell/stage.hpp>
#include <shell/surface.hpp>
#include <shell/texture.hpp>
#include <shell/transition.hpp>
#include <shell/vsync.hpp>

#ifdef SHELL_BACKEND_WAYLAND
#include <shell/backend/wayland.hpp>
#else
#include <shell/backend/drm.hpp>
#endif
