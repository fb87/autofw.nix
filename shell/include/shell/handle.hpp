#pragma once

#include <utility>

namespace shell
{

template<typename T, auto Release, T Invalid>
struct handle
{
    T value{Invalid};

    handle() = default;

    explicit handle(T new_value)
        : value{new_value}
    {
    }

    handle(const handle&) = delete;

    auto operator=(const handle&) -> handle& = delete;

    handle(handle&& other) noexcept
        : value{std::exchange(other.value, Invalid)}
    {
    }

    auto operator=(handle&& other) noexcept -> handle&
    {
        if (this == &other) {
            return *this;
        }

        reset();

        value = std::exchange(other.value, Invalid);

        return *this;
    }

    ~handle()
    {
        reset();
    }

    void reset()
    {
        if (value == Invalid) {
            return;
        }

        Release(value);
        value = Invalid;
    }

    [[nodiscard]]
    auto get() const -> T
    {
        return value;
    }
};

}
