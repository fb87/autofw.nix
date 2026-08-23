import 'package:flutter/widgets.dart';

abstract interface class ShellPlugin {
  String get id;

  Widget build(BuildContext context);

  Future<void> activate();

  Future<void> deactivate();
}
