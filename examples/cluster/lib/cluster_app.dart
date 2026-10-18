import 'package:flutter/material.dart';

import 'cluster_home.dart';

/// Digital EV instrument cluster. Hosted headlessly by shell::isolated_app
/// (see src/main.cpp's setup_dashboard_apps) and composited into the
/// dashboard app's left pane as a GL texture -- this app has no awareness
/// of being embedded (no shell/layers channel, no NativeSurface).
class ClusterApp extends StatelessWidget {
  const ClusterApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'shell cluster',
      theme: ThemeData.dark(),
      debugShowCheckedModeBanner: false,
      home: const ClusterHome(),
    );
  }
}
