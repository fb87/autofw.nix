/// shell::layer::id values the dashboard's two panes are keyed on. MUST
/// match src/main.cpp's kClusterLayerId/kInfotainmentLayerId exactly --
/// this is a hardcoded contract between the C++ side (which creates these
/// layers in setup_dashboard_apps) and this Dart app (which reads them back
/// over the 'shell/layers' channel), the same pattern every existing
/// texture/layer id in this codebase already uses.
const int kClusterLayerId = 10;
const int kInfotainmentLayerId = 11;
