#include <shell/shell.hpp>

int main()
{
    shell::layer::state layer{};
    shell::stage::state stage{};

    return (layer.value == 0 && stage.value == 0) ? 0 : 1;
}
