import 'package:flutter/widgets.dart';

class ShellLayer extends StatelessWidget {
  const ShellLayer({
    required this.child,
    this.x = 0,
    this.y = 0,
    this.width,
    this.height,
    this.visible = true,
    this.opacity = 1.0,
    super.key,
  });

  final Widget child;
  final double x;
  final double y;
  final double? width;
  final double? height;
  final bool visible;
  final double opacity;

  @override
  Widget build(BuildContext context) {
    if (!visible) {
      return const SizedBox.shrink();
    }

    return Positioned(
      left: x,
      top: y,
      width: width,
      height: height,
      child: Opacity(
        opacity: opacity,
        child: child,
      ),
    );
  }
}
