#pragma once

#include <utility>

#include <shell/error.hpp>

namespace shell
{

// A minimal std::expected<T, E> stand-in: C++23's <expected> is not available
// on older toolchains this project targets (e.g. GCC 11, C++20 only). Covers
// only the subset actually used here: default/value construction, boolean
// success check, value access, and error() on failure.
template <typename E> class unexpected
{
  public:
    constexpr explicit unexpected(E e) : e_(std::move(e))
    {
    }

    constexpr const E& error() const& noexcept
    {
        return e_;
    }
    constexpr E&& error() && noexcept
    {
        return std::move(e_);
    }

  private:
    E e_;
};

template <typename T, typename E> class expected
{
  public:
    constexpr expected() : ok_(true)
    {
    }
    constexpr expected(T v) : ok_(true), value_(std::move(v))
    {
    }
    constexpr expected(unexpected<E> u) : ok_(false), error_(std::move(u).error())
    {
    }

    constexpr explicit operator bool() const noexcept
    {
        return ok_;
    }

    constexpr const T& operator*() const& noexcept
    {
        return value_;
    }
    constexpr T& operator*() & noexcept
    {
        return value_;
    }
    constexpr T&& operator*() && noexcept
    {
        return std::move(value_);
    }

    constexpr const T* operator->() const noexcept
    {
        return &value_;
    }
    constexpr T* operator->() noexcept
    {
        return &value_;
    }

    constexpr const E& error() const& noexcept
    {
        return error_;
    }
    constexpr E&& error() && noexcept
    {
        return std::move(error_);
    }

  private:
    bool ok_;
    T value_{};
    E error_{};
};

template <typename E> class expected<void, E>
{
  public:
    constexpr expected() : ok_(true)
    {
    }
    constexpr expected(unexpected<E> u) : ok_(false), error_(std::move(u).error())
    {
    }

    constexpr explicit operator bool() const noexcept
    {
        return ok_;
    }

    constexpr const E& error() const& noexcept
    {
        return error_;
    }
    constexpr E&& error() && noexcept
    {
        return std::move(error_);
    }

  private:
    bool ok_;
    E error_{};
};

template <typename T> using result = expected<T, error>;

} // namespace shell
