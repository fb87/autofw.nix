#include <shell/shell.hpp>

int main()
{
    shell::stage::state main_stage{
        .value = 1,
        .size = {
            .width = 1280,
            .height = 720,
        },
        .view = {
            .state = shell::stage::visibility::visible,
            .progress = 1.0F,
        },
        .priority = 100,
    };

    shell::log::info("shell baseline");

    return main_stage.value == 1 ? 0 : 1;
}
