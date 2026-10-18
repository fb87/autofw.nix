import 'package:flutter/material.dart';

import 'layer_ids.dart';
import 'shell_pane_feed.dart';
import 'widgets/dock.dart';
import 'widgets/native_pane.dart';
import 'widgets/status_bar.dart';

/// Top-level HMI layout: the EV cluster full-height on the left; status
/// bar + infotainment + dock stacked on the right. The status bar and dock
/// are plain Flutter chrome inside this Column -- they're reserved,
/// fixed-height bands above/below the infotainment pane's Expanded, so it
/// is architecturally impossible for them to be drawn over either pane,
/// and the cluster side (the other half of the outer Row) never sees them
/// at all.
class DashboardHome extends StatefulWidget {
  const DashboardHome({super.key});

  @override
  State<DashboardHome> createState() => _DashboardHomeState();
}

class _DashboardHomeState extends State<DashboardHome> {
  final _feed = ShellPaneFeed();

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: Colors.black,
      body: LayoutBuilder(
        builder: (context, constraints) {
          final leftWidth = (constraints.maxWidth / 2).floorToDouble();
          return Row(
            children: [
              SizedBox(
                width: leftWidth,
                height: constraints.maxHeight,
                child: NativePane(
                  layerId: kClusterLayerId,
                  feed: _feed,
                  placeholder: 'cluster starting…',
                ),
              ),
              Expanded(
                child: Column(
                  children: [
                    const SizedBox(height: 56, child: StatusBar()),
                    Expanded(
                      child: NativePane(
                        layerId: kInfotainmentLayerId,
                        feed: _feed,
                        placeholder: 'infotainment starting…',
                      ),
                    ),
                    const SizedBox(height: 96, child: Dock()),
                  ],
                ),
              ),
            ],
          );
        },
      ),
    );
  }
}
