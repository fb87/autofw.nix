#include <shell/result.hpp>

#include <cassert>
#include <memory>
#include <string>

int main()
{
    shell::result<int> number = 42;
    assert(number);
    assert(number.has_value());
    assert(*number == 42);
    assert(number.value() == 42);

    shell::result<int> failed =
        shell::unexpected(shell::error{shell::error_code::invalid_argument, 7, "bad"});
    assert(!failed);
    assert(!failed.has_value());
    assert(failed.error().native_code == 7);
    assert(failed.error().message == "bad");

    shell::result<void> ok;
    assert(ok);
    ok.value();

    shell::result<void> void_failure =
        shell::unexpected(shell::error{shell::error_code::unavailable, 0, "missing"});
    assert(!void_failure);
    assert(void_failure.error().code == shell::error_code::unavailable);

    shell::result<std::unique_ptr<int>> moved = std::make_unique<int>(9);
    assert(**moved == 9);
    return 0;
}
