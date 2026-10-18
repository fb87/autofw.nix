import 'package:flutter/material.dart';

import '../model/vehicle_state.dart';

/// P / R / N / D selector readout. Purely data-driven off [mode] even
/// though the demo controller currently holds it fixed at [DriveMode.d].
class DriveModeIndicator extends StatelessWidget {
  const DriveModeIndicator({required this.mode, super.key});

  final DriveMode mode;

  static const _labels = {
    DriveMode.p: 'P',
    DriveMode.r: 'R',
    DriveMode.n: 'N',
    DriveMode.d: 'D',
  };

  @override
  Widget build(BuildContext context) {
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        for (final entry in _labels.entries) ...[
          _ModeChip(label: entry.value, active: entry.key == mode),
          if (entry.key != DriveMode.d) const SizedBox(width: 10),
        ],
      ],
    );
  }
}

class _ModeChip extends StatelessWidget {
  const _ModeChip({required this.label, required this.active});

  final String label;
  final bool active;

  @override
  Widget build(BuildContext context) {
    return Container(
      width: 32,
      height: 32,
      alignment: Alignment.center,
      decoration: BoxDecoration(
        color: active ? Colors.white : Colors.transparent,
        borderRadius: BorderRadius.circular(6),
      ),
      child: Text(
        label,
        style: TextStyle(
          color: active ? Colors.black : Colors.white38,
          fontSize: 18,
          fontWeight: FontWeight.w700,
        ),
      ),
    );
  }
}
