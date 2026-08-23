#pragma once

#include <expected>

#include <shell/error.hpp>

namespace shell
{

template <typename T> using result = std::expected<T, error>;

}
