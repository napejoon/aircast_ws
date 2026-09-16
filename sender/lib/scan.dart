import 'package:flutter/material.dart';
import 'package:mobile_scanner/mobile_scanner.dart';

import 'pairing.dart';

/// Full-screen camera, pops with a [PairingPayload] or with null.
///
/// It does not start the cast. Scanning fills the code and the server address
/// in and leaves the Start button to the user, because the server address is
/// the one field on this screen that decides where their screen is sent: a QR
/// on a poster is printed by whoever printed the poster, and a scan that cast
/// immediately would be a screen mirrored to a stranger's relay before its
/// owner had read the host it was going to.
class ScanPage extends StatefulWidget {
  const ScanPage({super.key});

  @override
  State<ScanPage> createState() => _ScanPageState();
}

class _ScanPageState extends State<ScanPage> {
  final _controller = MobileScannerController(
    // One format, so a barcode on a parcel in the same frame is not a result to
    // reject a hundred times a second.
    formats: const [BarcodeFormat.qrCode],
    detectionSpeed: DetectionSpeed.noDuplicates,
  );

  /// Pop once. Detections arrive in bursts and the first one has already left
  /// this route by the time the second lands.
  bool _done = false;

  /// What the last unusable scan was, or null. Shown rather than swallowed: a
  /// scanner that reads a code and does nothing is indistinguishable from one
  /// that cannot see it.
  String? _rejected;

  @override
  void dispose() {
    _controller.dispose();
    super.dispose();
  }

  void _onDetect(BarcodeCapture capture) {
    if (_done) return;
    for (final barcode in capture.barcodes) {
      final raw = barcode.rawValue;
      if (raw == null) continue;
      final payload = PairingPayload.parse(raw);
      if (payload != null) {
        _done = true;
        Navigator.of(context).pop(payload);
        return;
      }
      if (mounted) setState(() => _rejected = raw);
    }
  }

  @override
  Widget build(BuildContext context) => Scaffold(
        backgroundColor: Colors.black,
        body: Stack(
          fit: StackFit.expand,
          children: [
            MobileScanner(controller: _controller, onDetect: _onDetect),
            // The frame is guidance, not a crop: the scanner reads the whole
            // image, and a viewfinder that lies about that makes people hold
            // the phone further away than they need to.
            IgnorePointer(
              child: Center(
                child: Container(
                  width: 240,
                  height: 240,
                  decoration: BoxDecoration(
                    border: Border.all(color: const Color(0xFF3DDCD0), width: 2),
                    borderRadius: BorderRadius.circular(18),
                  ),
                ),
              ),
            ),
            SafeArea(
              child: Column(
                children: [
                  Align(
                    alignment: Alignment.topLeft,
                    child: IconButton(
                      icon: const Icon(Icons.close, color: Colors.white),
                      tooltip: 'Back',
                      onPressed: () => Navigator.of(context).pop(),
                    ),
                  ),
                  const Spacer(),
                  Padding(
                    padding: const EdgeInsets.all(24),
                    child: Text(
                      _rejected == null
                          ? 'Point this at the code on the desktop'
                          : 'That is not a Quoise pairing code',
                      textAlign: TextAlign.center,
                      style: TextStyle(
                        fontSize: 13,
                        color: _rejected == null
                            ? Colors.white70
                            : const Color(0xFFFFC857),
                      ),
                    ),
                  ),
                ],
              ),
            ),
          ],
        ),
      );
}
