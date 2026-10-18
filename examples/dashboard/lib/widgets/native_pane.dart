import 'package:flutter/material.dart';
import 'package:shell_flutter/native_surface.dart';

import '../shell_pane_feed.dart';

/// Shows the live NativeSurface for [layerId] when it has an attached
/// texture, or a placeholder otherwise. Watching `feed.textureIds` (rather
/// than hardcoding a permanent `NativeSurface(textureId: ...)`) is what
/// makes Step 5's blank-on-crash behavior actually visible here: when
/// shell::engine::set_surface_visible(false) takes the layer's reported
/// textureId to null, this unmounts the Texture subtree entirely -- no
/// frozen last frame -- and remounts once a respawned child reattaches.
class NativePane extends StatelessWidget {
  const NativePane({
    required this.layerId,
    required this.feed,
    required this.placeholder,
    super.key,
  });

  final int layerId;
  final ShellPaneFeed feed;
  final String placeholder;

  @override
  Widget build(BuildContext context) {
    return ValueListenableBuilder<Map<int, int?>>(
      valueListenable: feed.textureIds,
      builder: (context, map, _) {
        final textureId = map[layerId];
        if (textureId == null) {
          return Container(
            color: const Color(0xFF101010),
            alignment: Alignment.center,
            child: Text(
              placeholder,
              style: const TextStyle(color: Colors.white54),
            ),
          );
        }
        // shell::isolated_app's dma-buf-exported content renders vertically
        // flipped relative to Flutter's own texture-sampling convention
        // (confirmed empirically via a real composited BMP capture --
        // not something either shell::child_renderer's GL blit or the
        // GBM/EGL import step introduces on their own; most likely a
        // property of how Flutter's compositor samples an externally-
        // registered OpenGL texture generally). Compensated here, local to
        // this dashboard's own panes, rather than in the shared
        // NativeSurface widget: shell_flutter's NativeSurface is also used
        // by the gallery's CPU-pushed "External texture" demo
        // (shell::texture::push_texture_frame), a different producer path
        // that hasn't been verified to need the same correction, so a
        // blanket fix there risks flipping that demo instead of fixing it.
        return Transform(
          alignment: Alignment.center,
          transform: Matrix4.diagonal3Values(1, -1, 1),
          child: NativeSurface(textureId: textureId),
        );
      },
    );
  }
}
