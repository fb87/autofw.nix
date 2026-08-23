import 'package:flutter/widgets.dart';

class NativeSurface extends StatelessWidget {
  const NativeSurface({
    required this.textureId,
    super.key,
  });

  final int textureId;

  @override
  Widget build(BuildContext context) {
    return Texture(textureId: textureId);
  }
}
