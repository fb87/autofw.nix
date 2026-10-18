import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:shell_flutter/layer_host.dart' show ShellLayerData;

/// Tracks the live `shell/layers` snapshot, exposed as a plain
/// `layerId -> textureId` map (null = no surface currently attached to
/// that layer, i.e. shell::engine::set_surface_visible(false) during a
/// Step-5 crash/respawn gap, or the layer hasn't produced its first frame
/// yet). Deliberately bespoke here rather than added to the shared
/// shell_flutter package: this is a ~handful of lines with one consumer
/// (NativePane) tracking two known layer ids, not a generic abstraction --
/// shell_flutter's ShellLayerData parsing is reused as-is, only the
/// "render every layer as a debug placeholder box" part of ShellLayerHost
/// (which this app intentionally does NOT want) is skipped.
///
/// Uses a ValueNotifier instead of ShellLayerHost's setState-on-a-
/// StatefulWidget pattern: consumers subscribe directly via
/// ValueListenableBuilder, so only the affected pane rebuilds, and there's
/// no "if (!mounted) return" guard needed anywhere downstream.
class ShellPaneFeed {
  ShellPaneFeed() {
    _channel.setMethodCallHandler(_onMethodCall);
    _channel.invokeMethod<List<dynamic>>('list').then((result) {
      if (result != null) _apply(result);
    });
  }

  static const _channel = MethodChannel('shell/layers');

  final ValueNotifier<Map<int, int?>> textureIds = ValueNotifier(const {});

  Future<dynamic> _onMethodCall(MethodCall call) async {
    if (call.method == 'update') {
      _apply(call.arguments as List<dynamic>);
    }
    return null;
  }

  void _apply(List<dynamic> wire) {
    final map = <int, int?>{};
    for (final entry in wire) {
      final layer = ShellLayerData.fromWire(entry as List<dynamic>);
      map[layer.id] = layer.textureId;
    }
    textureIds.value = map;
  }
}
