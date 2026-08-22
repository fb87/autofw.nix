#include <flshell/flshell.hpp>

int main()
{
    flshell::stage::state main_stage{
        .value = 1,
        .size = {
            .width = 1280,
            .height = 720,
        },
        .view = {
            .state = flshell::stage::visibility::visible,
            .progress = 1.0F,
        },
        .priority = 100,
    };

    flshell::log::info("flshell baseline");

    return main_stage.value == 1 ? 0 : 1;
}
