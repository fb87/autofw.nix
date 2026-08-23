import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

/// Demonstrates external textures: a `shell/texture` method channel lets the
/// Dart side register an external texture with the embedder and push CPU frames
/// that Flutter composites through its rasterizer.
class TexturePanel extends StatefulWidget {
  const TexturePanel({super.key});

  @override
  State<TexturePanel> createState() => _TexturePanelState();
}

class _TexturePanelState extends State<TexturePanel> {
  static const _channel = MethodChannel('shell/texture');
  static const _kId = 1; // embedder-assigned texture id
  static const _size = 256;

  Timer? _timer;
  int _frame = 0;
  bool _pushing = false;

  @override
  void initState() {
    super.initState();
    _bootstrap();
  }

  @override
  void dispose() {
    _timer?.cancel();
    _channel.invokeMethod('unregister', _kId);
    super.dispose();
  }

  Future<void> _bootstrap() async {
    // Tell the embedder to allocate an external texture with this id.
    bool ok = false;
    try {
      ok = (await _channel.invokeMethod<bool>('register', _kId)) ?? false;
    } on PlatformException {
      ok = false; // texture bridge unavailable (GL-only build)
    }
    if (!mounted || !ok) return;
    setState(() => _pushing = true);
    // Push a frame every ~66 ms (~15 fps) to show a moving pattern.
    _timer = Timer.periodic(const Duration(milliseconds: 66), (_) {
      _pushFrame();
    });
  }

  Future<void> _pushFrame() async {
    final frame = _renderFrame(_frame++);
    try {
      await _channel.invokeMethod<bool?>('pushFrame', <Object?>[
        _kId,
        _size,
        _size,
        frame, // RGBA byte array
      ]);
    } on PlatformException {
      // Ignore transient failures; the panel stays live.
    }
  }

  // Compose an animated RGBA test pattern (moving color bars + a sweep).
  Uint8List _renderFrame(int n) {
    final p = Uint8List(_size * _size * 4);
    final t = n / 15.0; // seconds-ish
    for (var y = 0; y < _size; y++) {
      for (var x = 0; x < _size; x++) {
        final band = (x + (n * 6)) % _size;
        // Vertical color bars hue-shifting over time.
        var r = (band * 255 / _size).round();
        var g = ((x + y) / (2 * _size) * 255).round();
        var b = ((x * math.cos(t) + y * math.sin(t)).abs().round() % 256);
        // A bright diagonal sweep.
        final d = math.sqrt(math.pow(x - 128, 2) + math.pow(y - 128, 2));
        final onSweep = d < 40 + (n % 40);
        if (onSweep) {
          r = 255;
          g = 255;
          b = 255;
        }
        final i = (y * _size + x) * 4;
        p[i] = r;
        p[i + 1] = g;
        p[i + 2] = b;
        p[i + 3] = 255;
      }
    }
    return p;
  }

  @override
  Widget build(BuildContext context) {
    return ListView(
      padding: const EdgeInsets.all(24),
      children: [
        const Text(
          'shell/texture external texture',
          style: TextStyle(fontSize: 20),
        ),
        const SizedBox(height: 8),
        const Text(
          'The Dart side registers an external texture and pushes CPU-rendered '
          'RGBA frames that the embedder uploads and Flutter composites.',
        ),
        const SizedBox(height: 24),
        Center(
          child: ClipRRect(
            borderRadius: BorderRadius.circular(12),
            child: SizedBox(
              width: _size.toDouble(),
              height: _size.toDouble(),
              child: _pushing
                  ? Texture(textureId: _kId)
                  : const ColoredBox(color: Colors.black12),
            ),
          ),
        ),
        const SizedBox(height: 16),
        Center(
          child: Text(
            _pushing ? 'live · frame $_frame' : 'texture bridge unavailable',
          ),
        ),
      ],
    );
  }
}
