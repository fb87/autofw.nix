import 'package:flutter/widgets.dart';

class ShellVirtualKeyboard extends StatelessWidget {
  const ShellVirtualKeyboard({
    required this.onText,
    super.key,
  });

  final ValueChanged<String> onText;

  @override
  Widget build(BuildContext context) {
    return const SizedBox.shrink();
  }
}
