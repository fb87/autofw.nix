#pragma once

#include <optional>
#include <utility>
#include <variant>

#include <shell/error.hpp>

namespace shell
{

// C++20-compatible error carrier (replaces std::unexpected from C++23).
struct unexpected_error
{
    shell::error value;
};

inline auto unexpected(shell::error value) -> unexpected_error
{
    return {std::move(value)};
}

template <typename T> class result
{
  public:
    result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
    result(unexpected_error failure)
        : storage_(std::in_place_index<1>, std::move(failure.value))
    {
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return storage_.index() == 0;
    }

    [[nodiscard]] auto has_value() const noexcept -> bool
    {
        return storage_.index() == 0;
    }

    auto operator*() & -> T& { return std::get<0>(storage_); }
    auto operator*() const& -> const T& { return std::get<0>(storage_); }
    auto operator*() && -> T&& { return std::get<0>(std::move(storage_)); }
    auto operator->() -> T* { return &std::get<0>(storage_); }
    auto operator->() const -> const T* { return &std::get<0>(storage_); }

    auto value() & -> T& { return std::get<0>(storage_); }
    auto value() const& -> const T& { return std::get<0>(storage_); }
    auto error() & -> shell::error& { return std::get<1>(storage_); }
    auto error() const& -> const shell::error& { return std::get<1>(storage_); }

  private:
    std::variant<T, shell::error> storage_;
};

template <> class result<void>
{
  public:
    result() = default;
    result(unexpected_error failure) : error_(std::move(failure.value)) {}

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return !error_.has_value();
    }

    [[nodiscard]] auto has_value() const noexcept -> bool
    {
        return !error_.has_value();
    }

    auto value() const -> void
    {
        if (error_)
            throw std::bad_variant_access();
    }

    auto error() & -> shell::error& { return error_.value(); }
    auto error() const& -> const shell::error& { return error_.value(); }

  private:
    std::optional<shell::error> error_;
};

} // namespace shell
