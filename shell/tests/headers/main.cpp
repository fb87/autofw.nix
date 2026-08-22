#include <shell/shell.hpp>

// M0: all public headers compile independently and stage/layer API basics work.
int main()
{
    shell::stage::state stage{};
    stage.value = 1;
    stage.size = {1280, 720};
    shell::stage::set_visible(stage, true);
    if (stage.view.state != shell::stage::visibility::visible)
        return 1;
    if (!shell::stage::is_visible(stage))
        return 1;

    // Stage registry.
    shell::stage::registry stages;
    if (!shell::stage::add(stages, stage))
        return 1;
    if (!shell::stage::has(stages, 1))
        return 1;
    auto* got = shell::stage::get(stages, 1);
    if (got == nullptr || got->size.width != 1280)
        return 1;

    shell::layer::state layer{};
    layer.value = 10;
    layer.stage = 1;
    layer.kind = shell::layer::role::application;
    shell::layer::set_visible(layer, true);
    shell::layer::set_opacity(layer, 0.5F);
    shell::layer::set_input(layer, shell::layer::input_mode::normal);
    if (layer.opacity != 0.5F)
        return 1;
    if (!shell::layer::accepts_input(layer))
        return 1;

    // Default z-order policy: keyboard above application.
    if (shell::layer::z_of(shell::layer::role::application) >=
        shell::layer::z_of(shell::layer::role::keyboard))
        return 1;

    shell::layer::registry layers;
    if (!shell::layer::add(layers, layer))
        return 1;
    if (!shell::layer::has(layers, 10))
        return 1;

    return 0;
}
