import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

/// Demonstrates the `shell/demo` method channel handled by the embedder
/// (`echo` returns a canned reply; `ping` returns "pong").
class EchoPanel extends StatefulWidget {
  const EchoPanel({super.key});

  @override
  State<EchoPanel> createState() => _EchoPanelState();
}

class _EchoPanelState extends State<EchoPanel> {
  static const _channel = MethodChannel('shell/demo');

  String? _echo;
  String? _pong;

  Future<void> _callEcho() async {
    try {
      final result = await _channel.invokeMethod<String>('echo', 'hello-shell');
      if (!mounted) return;
      setState(() => _echo = result);
    } on PlatformException catch (e) {
      if (!mounted) return;
      setState(() => _echo = 'error: ${e.message}');
    }
  }

  Future<void> _callPing() async {
    try {
      final result = await _channel.invokeMethod<String>('ping');
      if (!mounted) return;
      setState(() => _pong = result);
    } on PlatformException catch (e) {
      if (!mounted) return;
      setState(() => _pong = 'error: ${e.message}');
    }
  }

  @override
  Widget build(BuildContext context) {
    return ListView(
      padding: const EdgeInsets.all(24),
      children: [
        const Text('shell/demo method channel', style: TextStyle(fontSize: 20)),
        const SizedBox(height: 8),
        const Text(
          'Round-trips a MethodChannel call into the embedder and back.',
        ),
        const SizedBox(height: 24),
        FilledButton.icon(
          onPressed: _callEcho,
          icon: const Icon(Icons.subdirectory_arrow_right),
          label: const Text('Call echo("hello-shell")'),
        ),
        if (_echo != null) ...[
          const SizedBox(height: 8),
          Text('echo -> $_echo'),
        ],
        const SizedBox(height: 16),
        FilledButton.icon(
          onPressed: _callPing,
          icon: const Icon(Icons.network_ping),
          label: const Text('Call ping()'),
        ),
        if (_pong != null) ...[
          const SizedBox(height: 8),
          Text('ping -> $_pong'),
        ],
      ],
    );
  }
}
