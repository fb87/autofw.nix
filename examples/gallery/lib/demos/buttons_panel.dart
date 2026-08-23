import 'package:flutter/material.dart';

import '../widgets/section_card.dart';

/// Every button variant: filled, elevated, outlined, text, tonal, icon,
/// floating action button, segmented and toggle chips.
class ButtonsPanel extends StatefulWidget {
  const ButtonsPanel({super.key});

  @override
  State<ButtonsPanel> createState() => _ButtonsPanelState();
}

class _ButtonsPanelState extends State<ButtonsPanel> {
  String _seg = 'left';
  bool _toggle = false;
  String _log = 'Tap a button';

  void _use(String what) => setState(() => _log = 'Pressed: $what');

  @override
  Widget build(BuildContext context) {
    return ListView(
      padding: const EdgeInsets.all(16),
      children: [
        SectionCard(title: 'Labeled buttons', children: [
          FilledButton(onPressed: () => _use('Filled'), child: const Text('Filled')),
          const SizedBox(height: 8),
          FilledButton.tonal(onPressed: () => _use('Filled tonal'), child: const Text('Filled tonal')),
          const SizedBox(height: 8),
          ElevatedButton(onPressed: () => _use('Elevated'), child: const Text('Elevated')),
          const SizedBox(height: 8),
          OutlinedButton(onPressed: () => _use('Outlined'), child: const Text('Outlined')),
          const SizedBox(height: 8),
          TextButton(onPressed: () => _use('Text'), child: const Text('Text')),
          const SizedBox(height: 8),
          FilledButton.icon(
            onPressed: () => _use('Filled.icon'),
            icon: const Icon(Icons.send),
            label: const Text('Filled.icon'),
          ),
        ]),
        SectionCard(title: 'Icon & floating buttons', children: [
          Wrap(spacing: 12, runSpacing: 12, children: [
            IconButton(onPressed: () => _use('IconButton'), icon: const Icon(Icons.star)),
            IconButton.filled(onPressed: () => _use('IconButton.filled'), icon: const Icon(Icons.favorite)),
            IconButton.outlined(onPressed: () => _use('IconButton.outlined'), icon: const Icon(Icons.search)),
            FloatingActionButton(
              onPressed: () => _use('FAB'),
              child: const Icon(Icons.add),
            ),
            FloatingActionButton.small(onPressed: () => _use('FAB.small'), child: const Icon(Icons.edit)),
            FloatingActionButton.extended(
              onPressed: () => _use('FAB.extended'),
              icon: const Icon(Icons.shopping_cart),
              label: const Text('extended'),
            ),
          ]),
        ]),
        SectionCard(title: 'Segmented & toggle', children: [
          SegmentedButton<String>(
            segments: const [
              ButtonSegment(value: 'left', label: Text('L'), icon: Icon(Icons.chevron_left)),
              ButtonSegment(value: 'right', label: Text('R'), icon: Icon(Icons.chevron_right)),
            ],
            selected: {_seg},
            onSelectionChanged: (s) => setState(() => _seg = s.first),
          ),
          const SizedBox(height: 12),
          FilterChip(
            label: const Text('Toggle chip'),
            selected: _toggle,
            onSelected: (v) => setState(() => _toggle = v),
          ),
          const SizedBox(height: 12),
          ToggleButtons(
            isSelected: const [true, false],
            onPressed: (i) => _use('ToggleButtons[$i]'),
            children: const [Icon(Icons.format_italic), Icon(Icons.format_bold)],
          ),
        ]),
        Card(
          child: Padding(
            padding: const EdgeInsets.all(12),
            child: Text(_log, style: Theme.of(context).textTheme.bodyLarge),
          ),
        ),
      ],
    );
  }
}
