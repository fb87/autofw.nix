import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

/// Demonstrates the shell::channel platform-message bridge from Flutter via a
/// custom MethodChannel (`shell/demo`) handled by the embedder.
void main() => runApp(const ChannelDemoApp());

class ChannelDemoApp extends StatelessWidget {
  const ChannelDemoApp({super.key});

  @override
  Widget build(BuildContext context) {
    return const MaterialApp(
      home: Scaffold(body: ChannelPanel()),
    );
  }
}

class ChannelPanel extends StatefulWidget {
  const ChannelPanel({super.key});

  @override
  State<ChannelPanel> createState() => _ChannelPanelState();
}

class _ChannelPanelState extends State<ChannelPanel> {
  static const MethodChannel _channel = MethodChannel('shell/demo');
  String? _echo;
  String? _pong;

  Future<void> _callEcho() async {
    try {
      final result = await _channel.invokeMethod<String>('echo', 'hello-shell');
      setState(() => _echo = result);
    } on PlatformException catch (e) {
      setState(() => _echo = 'error: ${e.message}');
    }
  }

  Future<void> _callPing() async {
    try {
      final result = await _channel.invokeMethod<String>('ping');
      setState(() => _pong = result);
    } on PlatformException catch (e) {
      setState(() => _pong = 'error: ${e.message}');
    }
  }

  @override
  Widget build(BuildContext context) {
    return Center(
      child: Column(
        mainAxisAlignment: MainAxisAlignment.center,
        children: [
          const Text('shell::channel demo', style: TextStyle(fontSize: 24)),
          const SizedBox(height: 24),
          FilledButton(onPressed: _callEcho, child: const Text('Call shell/demo echo')),
          if (_echo != null) Text('echo -> $_echo'),
          const SizedBox(height: 24),
          FilledButton(onPressed: _callPing, child: const Text('Call shell/demo ping')),
          if (_pong != null) Text('ping -> $_pong'),
        ],
      ),
    );
  }
}
