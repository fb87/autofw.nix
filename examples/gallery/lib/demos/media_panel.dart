import 'package:flutter/material.dart';

import '../widgets/section_card.dart';

/// Media-heavy widgets: an icon mosaic, Tooltip, InkWell ripple and a bundled
/// raster image.
class MediaPanel extends StatelessWidget {
  const MediaPanel({super.key});

  @override
  Widget build(BuildContext context) {
    return ListView(
      padding: const EdgeInsets.all(16),
      children: [
        SectionCard(title: 'Icon mosaic (from MaterialIcons)', children: [
          Wrap(
            spacing: 16,
            runSpacing: 16,
            children: const [
              Icon(Icons.home, size: 36),
              Icon(Icons.android, size: 36, color: Colors.green),
              Icon(Icons.flutter_dash, size: 36, color: Colors.blueAccent),
              Icon(Icons.lightbulb, size: 36, color: Colors.amber),
              Icon(Icons.favorite, size: 36, color: Colors.redAccent),
              Icon(Icons.dashboard, size: 36),
              Icon(Icons.star, size: 36, color: Colors.orange),
              Icon(Icons.settings, size: 36),
            ],
          ),
        ]),
        SectionCard(title: 'Image (bundled asset)', children: [
          Center(
            child: ClipRRect(
              borderRadius: BorderRadius.circular(12),
              child: Image.asset(
                'assets/images/sample.png',
                width: 220,
                height: 220,
                fit: BoxFit.cover,
              ),
            ),
          ),
        ]),
        SectionCard(title: 'Tooltip & InkWell', children: [
          Wrap(spacing: 24, children: const [
            Tooltip(
              message: 'Hover / long-press me',
              child: Icon(Icons.help_outline, size: 48),
            ),
            Tooltip(
              message: 'A labeled map pin',
              child: Icon(Icons.location_pin, size: 48, color: Colors.red),
            ),
          ]),
          const SizedBox(height: 12),
          Material(
            color: Theme.of(context).colorScheme.secondaryContainer,
            borderRadius: BorderRadius.circular(12),
            child: InkWell(
              borderRadius: BorderRadius.circular(12),
              onTap: () => ScaffoldMessenger.of(context).showSnackBar(
                  const SnackBar(content: Text('InkWell ripple tapped'))),
              child: const Padding(
                padding: EdgeInsets.all(20),
                child: Text('Tap for ripple (InkWell)'),
              ),
            ),
          ),
        ]),
      ],
    );
  }
}
