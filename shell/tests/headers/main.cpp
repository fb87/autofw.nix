#include <flshell/flshell.hpp>

int main()
{
    flshell::layer::state layer{};
    flshell::stage::state stage{};

    return (layer.value == 0 && stage.value == 0) ? 0 : 1;
}
