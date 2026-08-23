import 'package:flutter/widgets.dart';

import 'shell_plugin.dart';

class ShellApp extends StatelessWidget {
  const ShellApp({
    required this.plugins,
    required this.home,
    super.key,
  });

  final List<ShellPlugin> plugins;
  final Widget home;

  @override
  Widget build(BuildContext context) {
    return Directionality(
      textDirection: TextDirection.ltr,
      child: home,
    );
  }
}
