import 'dart:async';
import 'dart:convert';

import 'package:web_socket_channel/web_socket_channel.dart';

/// Wire protocol (see docs/protocol/signalling.md). One JSON object per frame,
/// every frame carries `type`. The sender is always the offerer.
///
///   out  {"type":"join","code":"123456","role":"sender"}
///   in   {"type":"joined","turn":{"urls":[...],"username":"...","credential":"..."}}
///   in   {"type":"peer"}                  receiver joined, safe to offer
///   out  {"type":"offer","sdp":"..."}
///   in   {"type":"answer","sdp":"..."}
///   both {"type":"candidate","candidate":{"candidate":"...","sdpMid":"...","sdpMLineIndex":0}}
///   both {"type":"bye"}
///   in   {"type":"error","message":"..."}
class IceServers {
  IceServers(this.urls, this.username, this.credential);

  final List<String> urls;
  final String username;
  final String credential;

  /// Relay is forced, so the TURN entry is the entire ICE configuration.
  Map<String, dynamic> toConfiguration() => {
        'iceServers': [
          {'urls': urls, 'username': username, 'credential': credential},
        ],
        'iceTransportPolicy': 'relay',
        'sdpSemantics': 'unified-plan',
      };
}

class Signaling {
  Signaling(this.url, this.code);

  final Uri url;
  final String code;

  WebSocketChannel? _channel;
  StreamSubscription<dynamic>? _sub;

  /// TURN credentials, minted per pairing by the server.
  final Completer<IceServers> _turn = Completer<IceServers>();
  Future<IceServers> get turn => _turn.future;

  /// Completes when the receiver has joined the same code.
  final Completer<void> _peer = Completer<void>();
  Future<void> get peerJoined => _peer.future;

  void Function(String sdp)? onAnswer;
  void Function(Map<String, dynamic> candidate)? onCandidate;
  void Function(String reason)? onClosed;

  Future<void> connect() async {
    // Whichever of these the caller does not await still gets an error on
    // failure, and an unlistened completer error is an uncaught async error —
    // fatal in a test, and a crash in the app.
    _turn.future.ignore();
    _peer.future.ignore();

    final channel = WebSocketChannel.connect(url);
    _channel = channel;
    await channel.ready;
    _sub = channel.stream.listen(
      _onFrame,
      onError: (Object e) => _fail('$e'),
      onDone: () => _fail('signalling connection closed'),
    );
    _send({'type': 'join', 'code': code, 'role': 'sender'});
  }

  void sendOffer(String sdp) => _send({'type': 'offer', 'sdp': sdp});

  void sendCandidate(Map<String, dynamic> candidate) =>
      _send({'type': 'candidate', 'candidate': candidate});

  void _onFrame(dynamic raw) {
    final Map<String, dynamic> msg;
    try {
      msg = jsonDecode(raw as String) as Map<String, dynamic>;
    } on Object {
      return _fail('malformed signalling frame');
    }
    switch (msg['type']) {
      case 'joined':
        final turn = msg['turn'] as Map<String, dynamic>?;
        if (turn == null) return _fail('server sent no TURN credentials');
        if (!_turn.isCompleted) {
          _turn.complete(IceServers(
            (turn['urls'] as List<dynamic>).cast<String>(),
            turn['username'] as String,
            turn['credential'] as String,
          ));
        }
      case 'peer':
        if (!_peer.isCompleted) _peer.complete();
      case 'answer':
        onAnswer?.call(msg['sdp'] as String);
      case 'candidate':
        onCandidate?.call((msg['candidate'] as Map).cast<String, dynamic>());
      case 'bye':
        _fail('the receiver hung up');
      case 'error':
        _fail(msg['message'] as String? ?? 'signalling error');
    }
  }

  void _fail(String reason) {
    if (!_turn.isCompleted) _turn.completeError(StateError(reason));
    if (!_peer.isCompleted) _peer.completeError(StateError(reason));
    onClosed?.call(reason);
  }

  void _send(Map<String, dynamic> msg) => _channel?.sink.add(jsonEncode(msg));

  Future<void> close() async {
    _send({'type': 'bye'});
    await _sub?.cancel();
    await _channel?.sink.close();
    _channel = null;
  }
}
