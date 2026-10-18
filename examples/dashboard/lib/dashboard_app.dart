import 'package:flutter/material.dart';

import 'dashboard_home.dart';

/// The primary engine's own content -- status bar, dock, and the two
/// composited panes (cluster, infotainment) hosted via shell::isolated_app.
/// See src/main.cpp's setup_dashboard_apps for the C++ side of this wiring.
class DashboardApp extends StatelessWidget {
  const DashboardApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'shell dashboard',
      theme: ThemeData.dark(),
      debugShowCheckedModeBanner: false,
      home: const DashboardHome(),
    );
  }
}
