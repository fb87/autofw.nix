/// Drive-selector position. Demo data holds this at [d] throughout --
/// flipping gears while "driving" would look wrong -- but the widgets stay
/// generic over the full enum so real vehicle data can drive it later.
enum DriveMode { p, r, n, d }

/// One snapshot of everything the cluster displays. Immutable -- a new
/// instance is pushed by the demo driver (or, eventually, real vehicle
/// data) on every tick; widgets rebuild off it via [ValueListenableBuilder].
class VehicleState {
  const VehicleState({
    required this.speedKmh,
    required this.powerFraction,
    required this.batteryPct,
    required this.rangeKm,
    required this.mode,
    required this.leftSignal,
    required this.rightSignal,
    required this.highBeam,
    required this.seatbeltWarn,
  });

  /// Vehicle speed in km/h.
  final double speedKmh;

  /// -1.0 (max regen) .. 0 (eco/coast) .. +1.0 (max power draw). EVs have no
  /// RPM/tachometer -- this is the equivalent "how hard is the drivetrain
  /// working, and in which direction" readout.
  final double powerFraction;

  /// Battery state of charge, 0..100.
  final double batteryPct;

  /// Estimated remaining range in km.
  final double rangeKm;

  final DriveMode mode;

  final bool leftSignal;
  final bool rightSignal;
  final bool highBeam;
  final bool seatbeltWarn;

  static const zero = VehicleState(
    speedKmh: 0,
    powerFraction: 0,
    batteryPct: 100,
    rangeKm: 370,
    mode: DriveMode.p,
    leftSignal: false,
    rightSignal: false,
    highBeam: false,
    seatbeltWarn: false,
  );
}
