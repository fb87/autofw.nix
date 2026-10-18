import 'package:flutter/material.dart';

/// The centerpiece: a large digital speed numeral with a unit label at its
/// baseline. Tabular figures keep the digit width constant as the value
/// changes, so the readout doesn't visibly jitter/reflow every tick.
class SpeedReadout extends StatelessWidget {
  const SpeedReadout({required this.speedKmh, super.key});

  final double speedKmh;

  @override
  Widget build(BuildContext context) {
    return Row(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.baseline,
      textBaseline: TextBaseline.alphabetic,
      children: [
        Text(
          speedKmh.round().toString(),
          style: const TextStyle(
            color: Colors.white,
            fontSize: 140,
            fontWeight: FontWeight.w700,
            height: 1.0,
            fontFeatures: [FontFeature.tabularFigures()],
          ),
        ),
        const SizedBox(width: 12),
        const Padding(
          padding: EdgeInsets.only(bottom: 18),
          child: Text(
            'km/h',
            style: TextStyle(color: Colors.white54, fontSize: 28, fontWeight: FontWeight.w500),
          ),
        ),
      ],
    );
  }
}
