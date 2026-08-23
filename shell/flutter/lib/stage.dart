import 'package:flutter/widgets.dart';

class ShellStage extends StatelessWidget {
  const ShellStage({
    required this.children,
    super.key,
  });

  final List<Widget> children;

  @override
  Widget build(BuildContext context) {
    return Stack(children: children);
  }
}
