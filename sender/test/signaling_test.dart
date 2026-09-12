import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:aircast_sender/signaling.dart';
import 'package:flutter_test/flutter_test.dart';

/// Drives the client against a real WebSocket, because the only thing worth
/// testing here is the frame handling.
void main() {
  test('join, TURN mint, peer arrival, answer and candidate relay', () async {
    final server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
    final joined = Completer<Map<String, dynamic>>();
    final offered = Completer<Map<String, dynamic>>();

    server.transform(WebSocketTransformer()).listen((socket) {
      socket.listen((raw) {
        final msg = jsonDecode(raw as String) as Map<String, dynamic>;
        switch (msg['type']) {
          case 'join':
            joined.complete(msg);
            socket.add(jsonEncode({
              'type': 'joined',
              'turn': {
                'urls': ['turn:relay.example.com:3478?transport=udp'],
                'username': '1757600000:abc',
                'credential': 'zm9v',
              },
            }));
            socket.add(jsonEncode({'type': 'peer'}));
          case 'offer':
            offered.complete(msg);
            socket.add(jsonEncode({'type': 'answer', 'sdp': 'v=0 answer'}));
            socket.add(jsonEncode({
              'type': 'candidate',
              'candidate': {'candidate': 'candidate:1 1 udp', 'sdpMid': '0', 'sdpMLineIndex': 0},
            }));
        }
      });
    });

    final signaling =
        Signaling(Uri.parse('ws://127.0.0.1:${server.port}'), '123456');
    final answer = Completer<String>();
    final candidate = Completer<Map<String, dynamic>>();
    signaling.onAnswer = answer.complete;
    signaling.onCandidate = candidate.complete;

    await signaling.connect();

    expect((await joined.future)['code'], '123456');
    expect((await joined.future)['role'], 'sender');

    final turn = await signaling.turn;
    expect(turn.username, '1757600000:abc');
    expect(turn.toConfiguration()['iceTransportPolicy'], 'relay');

    await signaling.peerJoined;
    signaling.sendOffer('v=0 offer');
    expect((await offered.future)['sdp'], 'v=0 offer');
    expect(await answer.future, 'v=0 answer');
    expect((await candidate.future)['sdpMid'], '0');

    await signaling.close();
    await server.close(force: true);
  });

  test('a server error fails the pending futures instead of hanging', () async {
    final server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
    server.transform(WebSocketTransformer()).listen((socket) {
      socket.listen((_) => socket.add(jsonEncode({'type': 'error', 'message': 'unknown code'})));
    });

    final signaling = Signaling(Uri.parse('ws://127.0.0.1:${server.port}'), '000000');
    await signaling.connect();

    await expectLater(signaling.turn, throwsA(isA<StateError>()));
    await signaling.close();
    await server.close(force: true);
  });
}
