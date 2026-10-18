import 'package:flutter/material.dart';

/// Bottom chrome strip, shown only over the infotainment pane. Decorative
/// app-icon row for this first pass -- not wired to switch content; the
/// dashboard currently has exactly one infotainment app (the Gallery)
/// composited in the pane above, so there is nothing yet for the dock to
/// switch between. Left as a visible, deliberately non-functional
/// placeholder rather than built out further.
class Dock extends StatelessWidget {
  const Dock({super.key});

  static const _icons = [
    Icons.map,
    Icons.music_note,
    Icons.phone,
    Icons.local_fire_department,
    Icons.settings,
  ];

  @override
  Widget build(BuildContext context) {
    return Container(
      color: const Color(0xFF1A1A1A),
      alignment: Alignment.center,
      child: Row(
        mainAxisAlignment: MainAxisAlignment.center,
        children: [
          for (final icon in _icons) ...[
            Container(
              width: 56,
              height: 56,
              decoration: BoxDecoration(
                color: Colors.white12,
                borderRadius: BorderRadius.circular(14),
              ),
              alignment: Alignment.center,
              child: Icon(icon, color: Colors.white70, size: 26),
            ),
            const SizedBox(width: 20),
          ],
        ],
      ),
    );
  }
}
