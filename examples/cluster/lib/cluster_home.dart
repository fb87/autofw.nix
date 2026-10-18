import 'package:flutter/material.dart';

import 'demo/demo_driver.dart';
import 'model/vehicle_state.dart';
import 'widgets/battery_gauge.dart';
import 'widgets/drive_mode_row.dart';
import 'widgets/power_meter.dart';
import 'widgets/speed_readout.dart';
import 'widgets/telltale_row.dart';

/// Assembles the whole cluster screen. One [ValueListenableBuilder] on the
/// demo controller drives every widget below it -- this is the single
/// animation clock for the entire app (see [ClusterDemoController]).
class ClusterHome extends StatefulWidget {
  const ClusterHome({super.key});

  @override
  State<ClusterHome> createState() => _ClusterHomeState();
}

class _ClusterHomeState extends State<ClusterHome> {
  final _controller = ClusterDemoController();

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    // Must be a Scaffold, not a bare Container -- a Container directly
    // under MaterialApp.home does not reliably get bounded constraints in
    // this embedder (observed empirically: Column/Row content collapsed to
    // its intrinsic size and mis-positioned at the extreme edges instead
    // of filling/centering within the screen). Scaffold's own layout
    // machinery provides the bounded constraints every child below
    // assumes.
    return Scaffold(
      backgroundColor: Colors.black,
      body: ValueListenableBuilder<VehicleState>(
        valueListenable: _controller,
        builder: (context, state, _) {
          return Column(
            children: [
              Expanded(
                flex: 5,
                child: Row(
                  children: [
                    SizedBox(
                      width: 120,
                      child: Padding(
                        padding: const EdgeInsets.only(left: 16),
                        child: Align(
                          alignment: Alignment.centerLeft,
                          child: BatteryGauge(
                              batteryPct: state.batteryPct,
                              rangeKm: state.rangeKm),
                        ),
                      ),
                    ),
                    Expanded(
                      child: Column(
                        mainAxisAlignment: MainAxisAlignment.center,
                        children: [
                          Padding(
                            padding: const EdgeInsets.symmetric(horizontal: 48),
                            child:
                                PowerMeter(powerFraction: state.powerFraction),
                          ),
                          const SizedBox(height: 8),
                          SpeedReadout(speedKmh: state.speedKmh),
                          const SizedBox(height: 16),
                          DriveModeIndicator(mode: state.mode),
                        ],
                      ),
                    ),
                    const SizedBox(width: 120),
                  ],
                ),
              ),
              SizedBox(
                height: 48,
                child: TelltaleRow(
                  leftSignal: state.leftSignal,
                  rightSignal: state.rightSignal,
                  highBeam: state.highBeam,
                  seatbeltWarn: state.seatbeltWarn,
                ),
              ),
              const SizedBox(height: 12),
            ],
          );
        },
      ),
    );
  }
}
