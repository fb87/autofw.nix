import 'package:flutter/material.dart';

/// A small battery-icon fill gauge plus the numeric state-of-charge and
/// estimated range -- the digital equivalent of a fuel gauge.
class BatteryGauge extends StatelessWidget {
  const BatteryGauge({required this.batteryPct, required this.rangeKm, super.key});

  final double batteryPct;
  final double rangeKm;

  Color get _fillColor {
    if (batteryPct < 20) return const Color(0xFFD32F2F);
    if (batteryPct < 50) return const Color(0xFFF9A825);
    return const Color(0xFF2E7D32);
  }

  @override
  Widget build(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      mainAxisSize: MainAxisSize.min,
      children: [
        SizedBox(
          width: 56,
          height: 24,
          child: CustomPaint(
            painter: _BatteryPainter(fraction: (batteryPct / 100).clamp(0.0, 1.0), color: _fillColor),
          ),
        ),
        const SizedBox(height: 6),
        Text(
          '${batteryPct.round()}%',
          style: const TextStyle(color: Colors.white, fontSize: 22, fontWeight: FontWeight.w600),
        ),
        Text(
          '${rangeKm.round()} km',
          style: const TextStyle(color: Colors.white54, fontSize: 14),
        ),
      ],
    );
  }
}

class _BatteryPainter extends CustomPainter {
  _BatteryPainter({required this.fraction, required this.color});

  final double fraction;
  final Color color;

  @override
  void paint(Canvas canvas, Size size) {
    const nubWidth = 4.0;
    final bodyRect = Rect.fromLTWH(0, 0, size.width - nubWidth, size.height);
    final body = RRect.fromRectAndRadius(bodyRect, const Radius.circular(4));

    canvas.drawRRect(body, Paint()..color = Colors.white24);
    canvas.drawRRect(
      body,
      Paint()
        ..color = Colors.white70
        ..style = PaintingStyle.stroke
        ..strokeWidth = 1.5,
    );

    final nub = RRect.fromRectAndRadius(
      Rect.fromLTWH(size.width - nubWidth, size.height * 0.25, nubWidth, size.height * 0.5),
      const Radius.circular(1.5),
    );
    canvas.drawRRect(nub, Paint()..color = Colors.white70);

    const inset = 3.0;
    final fillWidth = (bodyRect.width - inset * 2) * fraction;
    if (fillWidth > 0) {
      final fillRect = Rect.fromLTWH(inset, inset, fillWidth, bodyRect.height - inset * 2);
      canvas.drawRRect(RRect.fromRectAndRadius(fillRect, const Radius.circular(2)), Paint()..color = color);
    }
  }

  @override
  bool shouldRepaint(covariant _BatteryPainter oldDelegate) =>
      oldDelegate.fraction != fraction || oldDelegate.color != color;
}
