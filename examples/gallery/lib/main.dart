import 'package:flutter/material.dart';

import 'demos/calc_panel.dart';
import 'demos/echo_panel.dart';
import 'demos/texture_panel.dart';

/// Shell gallery: demonstrates shell:: components driven from a Flutter app.
///
/// Each entry maps to a panel that exercises one embedder bridge:
///   * method channel echo/ping (`shell/demo`)
///   * method channel with arguments (`shell/calc`)
///   * external textures (`shell/texture`)
void main() => runApp(const ShellGallery());

class ShellGallery extends StatelessWidget {
  const ShellGallery({super.key});

  @override
  Widget build(BuildContext context) {
    return const MaterialApp(
      title: 'shell gallery',
      home: GalleryHome(),
    );
  }
}

class GalleryHome extends StatefulWidget {
  const GalleryHome({super.key});

  @override
  State<GalleryHome> createState() => _GalleryHomeState();
}

class _GalleryHomeState extends State<GalleryHome> {
  int _index = 2; // start on the External-texture tab (flagship shell component)

  static const _titles = ['Channel echo', 'Calc args', 'External texture'];

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: Text('shell gallery · ${_titles[_index]}')),
      body: switch (_index) {
        0 => const EchoPanel(),
        1 => const CalcPanel(),
        2 => const TexturePanel(),
        _ => const SizedBox.shrink(),
      },
      bottomNavigationBar: NavigationBar(
        selectedIndex: _index,
        onDestinationSelected: (i) => setState(() => _index = i),
        destinations: const [
          NavigationDestination(
            icon: Icon(Icons.cable),
            label: 'Channel',
          ),
          NavigationDestination(
            icon: Icon(Icons.calculate),
            label: 'Calc',
          ),
          NavigationDestination(
            icon: Icon(Icons.layers),
            label: 'Texture',
          ),
        ],
      ),
    );
  }
}
