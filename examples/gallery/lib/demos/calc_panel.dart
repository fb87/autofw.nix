import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

/// Demonstrates passing an argument list to a `shell/calc` method channel the
/// embedder decodes (`add(a, b)` and `mul(a, b)` return an int).
class CalcPanel extends StatefulWidget {
  const CalcPanel({super.key});

  @override
  State<CalcPanel> createState() => _CalcPanelState();
}

class _CalcPanelState extends State<CalcPanel> {
  static const _channel = MethodChannel('shell/calc');

  int _a = 6;
  int _b = 7;
  String _sum = '—';
  String _product = '—';

  Future<void> _call(String method) async {
    try {
      final result =
          await _channel.invokeMethod<int>(method, <int>[_a, _b]);
      if (!mounted) return;
      setState(() {
        if (method == 'add') {
          _sum = '$result';
        } else {
          _product = '$result';
        }
      });
    } on PlatformException catch (e) {
      if (!mounted) return;
      setState(() {
        final msg = 'error: ${e.message}';
        if (method == 'add') {
          _sum = msg;
        } else {
          _product = msg;
        }
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    return ListView(
      padding: const EdgeInsets.all(24),
      children: [
        const Text('shell/calc method channel (arguments)',
            style: TextStyle(fontSize: 20)),
        const SizedBox(height: 8),
        const Text(
            'Sends [a, b] to the embedder, which decodes the int args and '
            'returns a computed value.'),
        const SizedBox(height: 24),
        Row(
          children: [
            Expanded(
              child: TextField(
                keyboardType: TextInputType.number,
                decoration: const InputDecoration(labelText: 'a'),
                onChanged: (v) => _a = int.tryParse(v) ?? 0,
              ),
            ),
            const SizedBox(width: 16),
            Expanded(
              child: TextField(
                keyboardType: TextInputType.number,
                decoration: const InputDecoration(labelText: 'b'),
                onChanged: (v) => _b = int.tryParse(v) ?? 0,
              ),
            ),
          ],
        ),
        const SizedBox(height: 24),
        FilledButton.icon(
          onPressed: () => _call('add'),
          icon: const Icon(Icons.add),
          label: const Text('add(a, b)'),
        ),
        const Text('sum -> '),
        Text(/* non-const */ _sum),
        const SizedBox(height: 12),
        FilledButton.icon(
          onPressed: () => _call('mul'),
          icon: const Icon(Icons.close),
          label: const Text('mul(a, b)'),
        ),
        const Text('product -> '),
        Text(_product),
      ],
    );
  }
}
