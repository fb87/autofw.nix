import 'package:flutter/material.dart';

/// Minor telltale/warning-light icons -- small, secondary, strip across the
/// bottom. Each is dimmed to near-invisible when inactive rather than
/// removed, matching how a real instrument panel's telltale row looks (the
/// icon positions are always present, only the illumination changes).
class TelltaleRow extends StatelessWidget {
  const TelltaleRow({
    required this.leftSignal,
    required this.rightSignal,
    required this.highBeam,
    required this.seatbeltWarn,
    super.key,
  });

  final bool leftSignal;
  final bool rightSignal;
  final bool highBeam;
  final bool seatbeltWarn;

  @override
  Widget build(BuildContext context) {
    return Row(
      mainAxisAlignment: MainAxisAlignment.center,
      children: [
        _Telltale(icon: Icons.arrow_back_ios, active: leftSignal, color: const Color(0xFF66BB6A)),
        const SizedBox(width: 28),
        _Telltale(icon: Icons.highlight, active: highBeam, color: const Color(0xFF42A5F5)),
        const SizedBox(width: 28),
        _Telltale(icon: Icons.warning_amber, active: seatbeltWarn, color: const Color(0xFFEF5350)),
        const SizedBox(width: 28),
        _Telltale(icon: Icons.arrow_forward_ios, active: rightSignal, color: const Color(0xFF66BB6A)),
      ],
    );
  }
}

class _Telltale extends StatelessWidget {
  const _Telltale({required this.icon, required this.active, required this.color});

  final IconData icon;
  final bool active;
  final Color color;

  @override
  Widget build(BuildContext context) {
    return Opacity(
      opacity: active ? 1.0 : 0.15,
      child: Icon(icon, size: 22, color: color),
    );
  }
}
