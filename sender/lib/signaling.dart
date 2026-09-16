import 'dart:async';
import 'dart:convert';

import 'package:web_socket_channel/web_socket_channel.dart';

/// Wire protocol (see docs/protocol/signalling.md). One JSON object per frame,
/// every frame carries `type`. The sender is always the offerer.
///
///   out  {"type":"join","code":"123456","role":"sender"}
///   in   {"type":"joined","turn":{"urls":[...],"username":"...","credential":"..."}}
///   in   {"type":"peer"}                  a receiver joined, offer to it
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
  ///
  /// [relayOnly] false is for LAN bring-up only: it lets ICE use host and
  /// server-reflexive candidates, which means the two peers learn each other's
  /// addresses. That is exactly what the product promises not to do, so it is
  /// off unless someone passes --dart-define=AIRCAST_RELAY=false.
  Map<String, dynamic> toConfiguration({bool relayOnly = true}) => {
        'iceServers': [
          {'urls': urls, 'username': username, 'credential': credential},
        ],
        'iceTransportPolicy': relayOnly ? 'relay' : 'all',
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

  /// The second `peer` frame, and every one after it.
  ///
  /// The server sends one whenever a receiver joins this code, and a receiver
  /// that loses its signalling socket mid-cast rejoins with the same code —
  /// the only one it has, the one printed on its own screen. What comes back
  /// is not the peer that answered: the receiver builds its pipeline once per
  /// socket, so it arrives with a webrtcbin seconds old, its own ICE
  /// credentials and its own DTLS certificate. The completer above is one-shot
  /// and start() awaits it exactly once, so until this callback existed the
  /// phone was told and did nothing at all: the desktop read "Negotiating" for
  /// ever while this app went on saying it was mirroring.
  void Function()? onPeerRejoined;

  Future<void> connect() async {
    // Whichever of these the caller does not await still gets an error on
    // failure, and an unlistened completer error is an uncaught async error —
    // fatal in a test, and a crash in the app.
    _turn.future.ignore();
    _peer.future.ignore();

    final channel = WebSocketChannel.connect(url);
    _channel = channel;
    // A wrong host or port hangs in SYN_SENT for the kernel's minutes-long
    // TCP timeout, and the UI sits on "connecting" with nothing in any log.
    // Ten seconds is generous for a TLS handshake anywhere; past it, fail
    // loudly with the URL so the mistake is readable.
    await channel.ready.timeout(const Duration(seconds: 10),
        onTimeout: () => throw TimeoutException('no answer from $url in 10 s'));
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
        // The first one is the receiver this cast was started for, and it
        // releases start(). Every one after it is a receiver that has to be
        // offered to again. _fail and close complete this same completer with
        // an error, so isCompleted is true on a dead session too, which is why
        // the session's handler checks that it still has a peer connection
        // before it offers anything.
        if (_peer.isCompleted) {
          onPeerRejoined?.call();
        } else {
          _peer.complete();
        }
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

  bool _failed = false;

  void _fail(String reason) {
    if (!_turn.isCompleted) _turn.completeError(StateError(reason));
    if (!_peer.isCompleted) _peer.completeError(StateError(reason));
    // Once. The server answers a bad code with an error frame and then closes,
    // so the real deployment delivers both an 'error' message and an onDone,
    // and each of them lands here. onClosed is wired to the teardown, so a
    // second call used to tear down whatever the user had started in between.
    if (_failed) return;
    _failed = true;
    onClosed?.call(reason);
  }

  void _send(Map<String, dynamic> msg) => _channel?.sink.add(jsonEncode(msg));

  Future<void> close() async {
    _send({'type': 'bye'});
    await _sub?.cancel();
    // Not awaited, and that is the point of the line. Until the socket is up
    // this sink is a StreamSinkCompleter buffering into a controller, and its
    // close() future does not complete until the real sink is handed over,
    // which for a connect that errored or is still waiting is never. _stop()
    // awaits this, so a mistyped server address left the window on
    // "Connecting" with a Stop button that did nothing and the address field
    // greyed out behind the casting flag, and only a force quit got out of it.
    // The close still happens if and when the connect resolves.
    _channel?.sink.close().ignore();
    _channel = null;
    // Cancelling the subscription above means no onDone and so no failure
    // callback, so a start() suspended on the TURN list or on peerJoined would
    // wait for a frame that can no longer arrive, for ever, still holding
    // whatever it had got as far as creating. Unwind it here instead. The
    // second teardown that unwind triggers is a no-op: _stop has already taken
    // the session out of its fields by the time this runs.
    if (!_turn.isCompleted) _turn.completeError(StateError('signalling closed'));
    if (!_peer.isCompleted) _peer.completeError(StateError('signalling closed'));
  }
}
