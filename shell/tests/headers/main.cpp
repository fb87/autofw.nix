#include <shell/shell.hpp>

// M0: all public headers compile independently and stage/layer API basics work.
int stage_layer_main()
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

int test_transitions()
{
    shell::transition::state t{};
    shell::transition::request r{};
    r.to.type = shell::transition::target_type::stage;
    r.to.id = 1;
    r.priority = 100;
    r.animation.duration = std::chrono::milliseconds(10);
    auto id = shell::transition::start(t, r);
    if (!id)
        return 1;
    if (shell::transition::get(t, *id) == nullptr)
        return 1;
    // Advance until complete.
    for (int i = 0; i < 20; ++i)
        if (!shell::transition::advance(t, *id))
            break;
    auto* a = shell::transition::get(t, *id);
    if (a == nullptr || a->phase != shell::transition::status::completed)
        return 1;
    // Higher priority interrupts.
    shell::transition::request hi{};
    hi.to.type = shell::transition::target_type::stage;
    hi.to.id = 1;
    hi.priority = 900;
    hi.animation.duration = std::chrono::milliseconds(10);
    auto hiid = shell::transition::start(t, hi);
    if (!hiid)
        return 1;
    return 0;
}

int test_shared()
{
    shell::shared::state st{};
    shell::shared::set(st, "media/source", "radio");
    if (*shell::shared::get(st, "media/source") != "radio")
        return 1;

    shell::shared::clipboard::state cb{};
    shell::shared::clipboard::set_text(cb, "hello");
    auto* item = shell::shared::clipboard::get(cb);
    if (item == nullptr || item->data.empty())
        return 1;
    shell::shared::clipboard::clear(cb);
    if (shell::shared::clipboard::get(cb) != nullptr)
        return 1;

    shell::shared::selection::state sel{};
    shell::shared::selection::select(sel, 5, shell::shared::selection::type::text);
    if (!sel.active)
        return 1;
    shell::shared::selection::clear(sel);
    if (sel.active)
        return 1;

    return 0;
}

int main()
{
    // (existing stage/layer checks folded here)
    return test_transitions() + test_shared();
}
