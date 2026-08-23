import 'package:flutter/material.dart';

import '../widgets/section_card.dart';

/// Feedback & transient surfaces: SnackBar, AlertDialog, SimpleDialog, modal
/// bottom sheet and progress indicators.
class FeedbackPanel extends StatefulWidget {
  const FeedbackPanel({super.key});

  @override
  State<FeedbackPanel> createState() => _FeedbackPanelState();
}

class _FeedbackPanelState extends State<FeedbackPanel> {
  bool _busy = false;

  Future<void> _simulateBusy() async {
    setState(() => _busy = true);
    await Future.delayed(const Duration(seconds: 2));
    if (mounted) setState(() => _busy = false);
  }

  void _snack(String text) {
    ScaffoldMessenger.of(context)
      ..hideCurrentSnackBar()
      ..showSnackBar(SnackBar(content: Text(text)));
  }

  Future<void> _showAlert() async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (c) => AlertDialog(
        title: const Text('Alert dialog'),
        content: const Text('This is an AlertDialog with actions.'),
        actions: [
          TextButton(
              onPressed: () => Navigator.pop(c, false),
              child: const Text('Cancel')),
          FilledButton(
              onPressed: () => Navigator.pop(c, true),
              child: const Text('Okay')),
        ],
      ),
    );
    _snack('AlertDialog ${ok == true ? 'accepted' : 'dismissed'}');
  }

  Future<void> _showSimple() async {
    final result = await showDialog<String>(
      context: context,
      builder: (c) => SimpleDialog(
        title: const Text('Pick one'),
        children: [
          SimpleDialogOption(
              child: const Text('Option A'), onPressed: () => Navigator.pop(c, 'A')),
          SimpleDialogOption(
              child: const Text('Option B'), onPressed: () => Navigator.pop(c, 'B')),
        ],
      ),
    );
    _snack('SimpleDialog -> $result');
  }

  Future<void> _showSheet() async {
    final choice = await showModalBottomSheet<String>(
      context: context,
      builder: (c) => SafeArea(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            ListTile(
                leading: const Icon(Icons.photo),
                title: const Text('Photo'),
                onTap: () => Navigator.pop(c, 'photo')),
            ListTile(
                leading: const Icon(Icons.camera_alt),
                title: const Text('Camera'),
                onTap: () => Navigator.pop(c, 'camera')),
          ],
        ),
      ),
    );
    _snack('Bottom sheet -> $choice');
  }

  @override
  Widget build(BuildContext context) {
    return ListView(
      padding: const EdgeInsets.all(16),
      children: [
        SectionCard(title: 'SnackBars', children: [
          FilledButton(
              onPressed: () => _snack('A temporary message'), child: const Text('Show SnackBar')),
          const SizedBox(height: 8),
          FilledButton.tonal(
              onPressed: () => _snack('With an action'),
              child: const Text('SnackBar + action')),
          const SizedBox(height: 8),
          FilledButton.tonal(
              onPressed: () => _snack('Floating variant'),
              child: const Text('Floating SnackBar')),
        ]),
        SectionCard(title: 'Dialogs & sheets', children: [
          OutlinedButton(onPressed: _showAlert, child: const Text('AlertDialog')),
          const SizedBox(height: 8),
          OutlinedButton(onPressed: _showSimple, child: const Text('SimpleDialog')),
          const SizedBox(height: 8),
          OutlinedButton(onPressed: _showSheet, child: const Text('Modal bottom sheet')),
        ]),
        SectionCard(title: 'Progress indicators', children: [
          Row(
            children: [
              const CircularProgressIndicator(),
              const SizedBox(width: 24),
              _busy ? const SizedBox(width: 24, height: 24, child: CircularProgressIndicator()) : const Icon(Icons.done, color: Colors.green),
              const SizedBox(width: 24),
              const LinearProgressIndicator(),
            ],
          ),
          const SizedBox(height: 18),
          FilledButton(
            onPressed: _busy ? null : _simulateBusy,
            child: Text(_busy ? 'Working…' : 'Simulate 2s work'),
          ),
        ]),
      ],
    );
  }
}
