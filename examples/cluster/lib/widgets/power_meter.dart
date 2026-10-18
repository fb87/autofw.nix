import 'package:flutter/material.dart';

/// EVs have no tachometer -- this is the equivalent "how hard is the
/// drivetrain working" readout: a horizontal gradient bar from regen
/// (green, left) through eco (gray, center) to power draw (amber/red,
/// right), with a bright moving indicator bar marking the current value.
/// Deliberately a bar + indicator, not a needle -- this is a flat/digital
/// instrument, not an analog gauge.
class PowerMeter extends StatelessWidget {
  const PowerMeter({required this.powerFraction, super.key});

  /// -1.0 (max regen) .. 0 (eco) .. +1.0 (max power).
  final double powerFraction;

  @override
  Widget build(BuildContext context) {
    return Column(
      mainAxisSize: MainAxisSize.min,
      children: [
        SizedBox(
          height: 28,
          width: double.infinity,
          child: CustomPaint(
            painter: _PowerMeterPainter(powerFraction: powerFraction.clamp(-1.0, 1.0)),
          ),
        ),
        const SizedBox(height: 4),
        const Padding(
          padding: EdgeInsets.symmetric(horizontal: 4),
          child: Row(
            mainAxisAlignment: MainAxisAlignment.spaceBetween,
            children: [
              Text('REGEN', style: TextStyle(color: Color(0xFF2E7D32), fontSize: 11, fontWeight: FontWeight.w600)),
              Text('ECO', style: TextStyle(color: Colors.white38, fontSize: 11, fontWeight: FontWeight.w600)),
              Text('POWER', style: TextStyle(color: Color(0xFFD32F2F), fontSize: 11, fontWeight: FontWeight.w600)),
            ],
          ),
        ),
      ],
    );
  }
}

class _PowerMeterPainter extends CustomPainter {
  _PowerMeterPainter({required this.powerFraction});

  final double powerFraction;

  @override
  void paint(Canvas canvas, Size size) {
    final track = RRect.fromRectAndRadius(
      Offset.zero & size,
      Radius.circular(size.height / 2),
    );

    final trackPaint = Paint()
      ..shader = const LinearGradient(
        colors: [Color(0xFF2E7D32), Color(0xFF455A64), Color(0xFFF9A825), Color(0xFFD32F2F)],
        stops: [0.0, 0.5, 0.75, 1.0],
      ).createShader(Offset.zero & size);
    canvas.drawRRect(track, trackPaint);

    // Dim overlay so the gradient reads as a "track", with the bright
    // indicator bar standing out as the actual current value.
    canvas.drawRRect(track, Paint()..color = Colors.black.withValues(alpha: 0.35));

    final x = ((powerFraction + 1) / 2) * size.width;
    final indicator = RRect.fromRectAndRadius(
      Rect.fromCenter(center: Offset(x, size.height / 2), width: 5, height: size.height + 6),
      const Radius.circular(2.5),
    );
    canvas.drawRRect(indicator, Paint()..color = Colors.white);
  }

  @override
  bool shouldRepaint(covariant _PowerMeterPainter oldDelegate) =>
      oldDelegate.powerFraction != powerFraction;
}
