import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/foundation.dart';

import '../model/vehicle_state.dart';

/// No real vehicle bus exists in this environment, so the cluster is driven
/// by a single 100ms timer producing a smooth, plausible 20s drive cycle:
/// speed ramps 0 -> 120 -> 0, power draw is phase-locked to it (positive
/// while accelerating, negative/regen while decelerating), and turn
/// signals/high-beam pulse at fixed points in the same cycle so the whole
/// cluster visibly "feels" like one coherent drive, not independent random
/// values.
class ClusterDemoController extends ValueNotifier<VehicleState> {
  ClusterDemoController() : super(VehicleState.zero) {
    _ticker = Timer.periodic(const Duration(milliseconds: 100), _tick);
    _blinkTicker = Timer.periodic(const Duration(milliseconds: 500), _blink);
  }

  static const _cycleSeconds = 20.0;
  static const _ratedRangeKm = 370.0;

  late final Timer _ticker;
  late final Timer _blinkTicker;
  double _elapsedSeconds = 0;
  bool _blinkOn = true;

  void _tick(Timer timer) {
    _elapsedSeconds += 0.1;
    final phase = (_elapsedSeconds % _cycleSeconds) / _cycleSeconds; // 0..1

    final speed = 60 - 60 * math.cos(2 * math.pi * phase);
    final power = math.sin(2 * math.pi * phase);

    // Battery/range tick down very slowly -- purely cosmetic, just enough
    // to prove the readout is live, never meant to look dramatic.
    final batteryPct = math.max(15.0, 84.0 - _elapsedSeconds / 3600 * 100 * 0.2);
    final rangeKm = batteryPct / 100 * _ratedRangeKm;

    final leftOn = phase < 0.2;
    final highBeamOn = phase >= 0.4 && phase < 0.5;
    final seatbeltWarn = _elapsedSeconds < 2;

    value = VehicleState(
      speedKmh: speed,
      powerFraction: power,
      batteryPct: batteryPct,
      rangeKm: rangeKm,
      mode: DriveMode.d,
      leftSignal: leftOn && _blinkOn,
      rightSignal: false,
      highBeam: highBeamOn,
      seatbeltWarn: seatbeltWarn,
    );
  }

  void _blink(Timer timer) {
    _blinkOn = !_blinkOn;
  }

  @override
  void dispose() {
    _ticker.cancel();
    _blinkTicker.cancel();
    super.dispose();
  }
}
