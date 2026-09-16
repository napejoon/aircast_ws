import 'package:aircast_sender/pairing.dart';
import 'package:flutter_test/flutter_test.dart';

/// The parser reads whatever a camera happened to be pointed at, and what it
/// accepts decides where this phone's screen is sent. Everything here is a
/// string a scanner can plausibly hand it.
void main() {
  test('a pairing URL gives up its code and its server', () {
    final p = PairingPayload.parse('aircast://pair?c=482913&s=wss%3A%2F%2Fhost.example%2Fws');
    expect(p, isNotNull);
    expect(p!.code, '482913');
    expect(p.url, 'wss://host.example/ws');
  });

  test('ws:// is taken too, because LAN bring-up has no certificate', () {
    final p = PairingPayload.parse('aircast://pair?c=000001&s=ws%3A%2F%2F192.168.1.20%3A8443%2Fws');
    expect(p?.url, 'ws://192.168.1.20:8443/ws');
  });

  test('surrounding whitespace survives a scan', () {
    expect(
      PairingPayload.parse('  aircast://pair?c=123456&s=wss%3A%2F%2Fh%2Fws  ')?.code,
      '123456',
    );
  });

  for (final raw in <String>[
    'https://example.com/pair?c=123456&s=wss://h/ws', // someone else's QR
    'aircast://join?c=123456&s=wss://h/ws', // our scheme, not our host
    'aircast://pair?s=wss://h/ws', // no code
    'aircast://pair?c=12345&s=wss://h/ws', // five digits
    'aircast://pair?c=1234567&s=wss://h/ws', // seven
    'aircast://pair?c=12345a&s=wss://h/ws', // not digits
    'aircast://pair?c=+12345&s=wss://h/ws', // int.tryParse would take this
    'aircast://pair?c=123456', // no server
    'aircast://pair?c=123456&s=https%3A%2F%2Fh%2Fws', // not a websocket
    'aircast://pair?c=123456&s=wss%3A%2F%2F%2Fws', // no host
    'aircast://pair?c=123456&s=', // empty server
    'just some text on a poster',
    '',
  ]) {
    test('refused: $raw', () => expect(PairingPayload.parse(raw), isNull));
  }
}
