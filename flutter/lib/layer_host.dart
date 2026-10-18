import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

import 'layer.dart';
import 'native_surface.dart';
import 'stage.dart';

/// One decoded `shell/layers` entry. Field order matches
/// `shell::engine::encode_layer` (include/shell/engine_impl.hpp) exactly:
/// [id, stage, role, x*1000, y*1000, width*1000, height*1000,
///  opacity*1000, visible, z, textureId].
class ShellLayerData {
  const ShellLayerData({
    required this.id,
    required this.stage,
    required this.role,
    required this.x,
    required this.y,
    required this.width,
    required this.height,
    required this.opacity,
    required this.visible,
    required this.z,
    required this.textureId,
  });

  factory ShellLayerData.fromWire(List<dynamic> fields) {
    double scaled(int i) => (fields[i] as int) / 1000.0;
    final rawTextureId = fields[10] as int;
    return ShellLayerData(
      id: fields[0] as int,
      stage: fields[1] as int,
      role: fields[2] as int,
      x: scaled(3),
      y: scaled(4),
      width: scaled(5),
      height: scaled(6),
      opacity: scaled(7),
      visible: fields[8] as bool,
      z: fields[9] as int,
      // -1 is the "no surface attached" sentinel (shell::surface::id is
      // unsigned on the C++ side; -1 is never a real id).
      textureId: rawTextureId < 0 ? null : rawTextureId,
    );
  }

  final int id;
  final int stage;
  final int role;
  final double x;
  final double y;
  final double width;
  final double height;
  final double opacity;
  final bool visible;
  final int z;
  // Step 3: set when a secondary engine's surface is attached to this
  // layer (shell::engine::attach_surface). Null means "no live surface
  // content yet" -- the placeholder box is shown instead.
  final int? textureId;
}

// Mirrors shell::layer::role (include/shell/layer.hpp) by ordinal.
const List<String> _roleNames = [
  'background',
  'application',
  'system_ui',
  'overlay',
  'notification',
  'keyboard',
  'emergency',
];

const List<Color> _roleColors = [
  Color(0xFF37474F), // background
  Color(0xFF1565C0), // application
  Color(0xFF6A1B9A), // system_ui
  Color(0xFFEF6C00), // overlay
  Color(0xFFC62828), // notification
  Color(0xFF2E7D32), // keyboard
  Color(0xFFB71C1C), // emergency
];

/// Listens on the `shell/layers` channel (shell::engine::push_layers /
/// the "list" method in include/shell/engine_impl.hpp) and renders the
/// current layer set as a `ShellStage` of role-colored, id-labeled
/// placeholder boxes -- proves the C++ layer registry actually drives
/// on-screen compositing. Hosting real plugin/app content per layer is a
/// later step; this is deliberately just a visible placeholder.
class ShellLayerHost extends StatefulWidget {
  const ShellLayerHost({super.key});

  @override
  State<ShellLayerHost> createState() => _ShellLayerHostState();
}

class _ShellLayerHostState extends State<ShellLayerHost> {
  static const _channel = MethodChannel('shell/layers');

  List<ShellLayerData> _layers = const [];

  @override
  void initState() {
    super.initState();
    _channel.setMethodCallHandler(_onMethodCall);
    _fetchInitialSnapshot();
  }

  Future<void> _fetchInitialSnapshot() async {
    try {
      final result = await _channel.invokeMethod<List<dynamic>>('list');
      if (result != null) {
        _applySnapshot(result);
      }
    } on PlatformException catch (e) {
      debugPrint('[layer] initial snapshot failed: ${e.message}');
    }
  }

  Future<dynamic> _onMethodCall(MethodCall call) async {
    if (call.method == 'update') {
      _applySnapshot(call.arguments as List<dynamic>);
    }
    return null;
  }

  void _applySnapshot(List<dynamic> wire) {
    final layers = wire
        .map((entry) => ShellLayerData.fromWire(entry as List<dynamic>))
        .toList();
    for (final l in layers) {
      debugPrint('[layer] id=${l.id} role=${_roleNames[l.role]} '
          'visible=${l.visible} opacity=${l.opacity.toStringAsFixed(2)} '
          'bounds=(${l.x},${l.y},${l.width}x${l.height}) z=${l.z} '
          'textureId=${l.textureId}');
    }
    if (!mounted) return;
    setState(() => _layers = layers);
  }

  @override
  Widget build(BuildContext context) {
    return IgnorePointer(
      child: ShellStage(
        children: [
          for (final l in _layers)
            ShellLayer(
              key: ValueKey(l.id),
              x: l.x,
              y: l.y,
              width: l.width,
              height: l.height,
              visible: l.visible,
              opacity: l.opacity,
              child: l.textureId != null
                  ? NativeSurface(textureId: l.textureId!)
                  : Container(
                      color: _roleColors[l.role].withValues(alpha: 0.65),
                      alignment: Alignment.center,
                      child: Text(
                        'layer ${l.id}\n${_roleNames[l.role]}',
                        textAlign: TextAlign.center,
                        style: const TextStyle(color: Color(0xFFFFFFFF)),
                      ),
                    ),
            ),
        ],
      ),
    );
  }
}
