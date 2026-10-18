import 'dart:async';

import 'package:flutter/material.dart';

/// Top chrome strip, shown only over the infotainment pane -- not part of
/// the infotainment app itself, not shown over the cluster side (see
/// DashboardHome's layout: this lives entirely inside the right column).
class StatusBar extends StatefulWidget {
  const StatusBar({super.key});

  @override
  State<StatusBar> createState() => _StatusBarState();
}

class _StatusBarState extends State<StatusBar> {
  late final Timer _clockTimer;
  DateTime _now = DateTime.now();

  @override
  void initState() {
    super.initState();
    _clockTimer = Timer.periodic(const Duration(seconds: 1), (_) {
      setState(() => _now = DateTime.now());
    });
  }

  @override
  void dispose() {
    _clockTimer.cancel();
    super.dispose();
  }

  String get _time =>
      '${_now.hour.toString().padLeft(2, '0')}:${_now.minute.toString().padLeft(2, '0')}';

  @override
  Widget build(BuildContext context) {
    return Container(
      color: const Color(0xFF1A1A1A),
      padding: const EdgeInsets.symmetric(horizontal: 20),
      alignment: Alignment.center,
      child: Row(
        children: [
          Text(_time, style: const TextStyle(color: Colors.white, fontSize: 18, fontWeight: FontWeight.w600)),
          const Spacer(),
          const Icon(Icons.wifi, color: Colors.white70, size: 20),
          const SizedBox(width: 16),
          const Icon(Icons.bluetooth, color: Colors.white70, size: 20),
          const SizedBox(width: 16),
          const Icon(Icons.signal_cellular_alt, color: Colors.white70, size: 20),
        ],
      ),
    );
  }
}
