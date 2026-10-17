import 'package:flutter/material.dart';
import 'package:shell_flutter/layer_host.dart';

import 'demos/buttons_panel.dart';
import 'demos/calc_panel.dart';
import 'demos/echo_panel.dart';
import 'demos/feedback_panel.dart';
import 'demos/form_panel.dart';
import 'demos/media_panel.dart';
import 'demos/selection_panel.dart';
import 'demos/texture_panel.dart';

/// Shell gallery: a tour of shell:: components driven from a Flutter app.
///
/// Each drawer entry maps to a panel that exercises one or more Flutter
/// components (inputs, buttons, selection, feedback, media) and the embedder
/// bridges (method channels, argument passing, external textures).
void main() => runApp(const ShellGallery());

class ShellGallery extends StatelessWidget {
  const ShellGallery({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'shell gallery',
      theme: MaterialTheme.light,
      darkTheme: MaterialTheme.dark,
      home: const GalleryHome(),
    );
  }
}

/// Minimal Material 3 themes so the gallery renders with a consistent look.
class MaterialTheme {
  static const _seed = Color(0xFF6750A4);
  static ThemeData get light =>
      ThemeData(colorScheme: ColorScheme.fromSeed(seedColor: _seed));
  static ThemeData get dark =>
      ThemeData(colorScheme: ColorScheme.fromSeed(
        seedColor: _seed,
        brightness: Brightness.dark,
      ));
}

/// A drawer destination: title, subtitle, icon and the panel widget.
class _Entry {
  const _Entry(this.title, this.subtitle, this.icon, this.panel);
  final String title;
  final String subtitle;
  final IconData icon;
  final Widget panel;
}

const List<_Entry> _entries = [
  _Entry('Form & inputs', 'TextField, checkbox, radio, switch, slider',
      Icons.input, FormPanel()),
  _Entry('Buttons', 'Filled, elevated, outlined, FAB, segmented',
      Icons.smart_button, ButtonsPanel()),
  _Entry('Selection & lists', 'Card, ListTile, chip, data table',
      Icons.list, SelectionPanel()),
  _Entry('Feedback', 'SnackBar, dialog, bottom sheet, progress',
      Icons.notifications_active, FeedbackPanel()),
  _Entry('Media & icons', 'Icons, image, tooltip, ink ripple',
      Icons.image, MediaPanel()),
  _Entry('Channel echo', 'shell/demo method channel', Icons.cable, EchoPanel()),
  _Entry('Calc arguments', 'shell/calc int args', Icons.calculate, CalcPanel()),
  _Entry('External texture', 'shell/texture GL frames', Icons.layers,
      TexturePanel()),
];

class GalleryHome extends StatefulWidget {
  const GalleryHome({super.key});

  @override
  State<GalleryHome> createState() => _GalleryHomeState();
}

class _GalleryHomeState extends State<GalleryHome> {
  int _index = 0; // start on Form & inputs (showcases typed text input)

  @override
  Widget build(BuildContext context) {
    final sel = _entries[_index];
    return Scaffold(
      appBar: AppBar(title: Text('shell gallery · ${sel.title}')),
      drawer: Drawer(
        child: ListView(
          children: [
            const DrawerHeader(
              decoration: BoxDecoration(color: Colors.deepPurple),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Icon(Icons.widgets, size: 44, color: Colors.white),
                  SizedBox(height: 8),
                  Text('shell gallery', style: TextStyle(fontSize: 22, color: Colors.white, fontWeight: FontWeight.bold)),
                  Text('Flutter components + embedder bridges',
                      style: TextStyle(color: Colors.white70)),
                ],
              ),
            ),
            for (final e in _entries)
              ListTile(
                leading: Icon(e.icon),
                title: Text(e.title),
                subtitle: Text(e.subtitle),
                selected: e.title == sel.title,
                onTap: () {
                  setState(() => _index = _entries.indexOf(e));
                  Navigator.pop(context);
                },
              ),
          ],
        ),
      ),
      body: Stack(
        children: [
          sel.panel,
          const ShellLayerHost(),
        ],
      ),
    );
  }
}
