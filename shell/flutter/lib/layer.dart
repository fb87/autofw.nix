import 'package:flutter/widgets.dart';

class ShellLayer extends StatelessWidget {
  const ShellLayer({
    required this.child,
    this.visible = true,
    this.opacity = 1.0,
    super.key,
  });

  final Widget child;
  final bool visible;
  final double opacity;

  @override
  Widget build(BuildContext context) {
    if (!visible) {
      return const SizedBox.shrink();
    }

    return Opacity(
      opacity: opacity,
      child: child,
    );
  }
}
