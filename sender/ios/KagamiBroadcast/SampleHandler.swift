// The Broadcast Upload Extension's entry point: ReplayKit hands it every frame
// of the whole screen, and it passes them to the Kagami app over a UNIX socket
// in the App Group container, where flutter_webrtc's
// FlutterBroadcastScreenCapturer turns them into the WebRTC track.
//
// Adapted from LiveKit's client-sdk-flutter example (Apache-2.0), itself from
// Jitsi Meet. tools/scaffold-ios.sh fetches the other four files of that
// extension -- SampleUploader, SocketConnection, DarwinNotificationCenter,
// Atomic -- at a pinned commit, checked by hash. This one is ours because it is
// the file that names the App Group, and it reads it from this extension's
// Info.plist rather than writing it here: the same string sits in the app's
// Info.plist (flutter_webrtc reads it there) and in both entitlements, and a
// fourth copy in code is the one that would drift.

import OSLog
import ReplayKit

let broadcastLogger = OSLog(subsystem: "io.kagami.sender.broadcast", category: "Broadcast")

class SampleHandler: RPBroadcastSampleHandler {

    private var clientConnection: SocketConnection?
    private var uploader: SampleUploader?

    private var socketFilePath: String {
        guard
            let group = Bundle.main.object(forInfoDictionaryKey: "RTCAppGroupIdentifier") as? String,
            let container = FileManager.default.containerURL(
                forSecurityApplicationGroupIdentifier: group)
        else {
            // No group means no socket, and a broadcast that runs while sending
            // nowhere. openConnection() then never succeeds and the picker's
            // red bar is the only sign, so say it where Console can see it.
            os_log(.error, log: broadcastLogger, "no App Group container; check the entitlements")
            return ""
        }
        return container.appendingPathComponent("rtc_SSFD").path
    }

    override init() {
        super.init()
        if let connection = SocketConnection(filePath: socketFilePath) {
            clientConnection = connection
            setupConnection()
            uploader = SampleUploader(connection: connection)
        }
    }

    override func broadcastStarted(withSetupInfo setupInfo: [String: NSObject]?) {
        // Also the app's cue to keep itself running in the background
        // (Runner/KagamiKeepAlive.m): the frames go through it.
        DarwinNotificationCenter.shared.postNotification(.broadcastStarted)
        openConnection()
    }

    override func broadcastFinished() {
        DarwinNotificationCenter.shared.postNotification(.broadcastStopped)
        clientConnection?.close()
    }

    override func processSampleBuffer(_ sampleBuffer: CMSampleBuffer,
                                      with sampleBufferType: RPSampleBufferType) {
        if sampleBufferType == .video {
            uploader?.send(sample: sampleBuffer)
        }
    }
}

private extension SampleHandler {

    func setupConnection() {
        clientConnection?.didClose = { [weak self] error in
            if let error = error {
                self?.finishBroadcastWithError(error)
            } else {
                // The app closed its end: the cast stopped there.
                let stopped = NSError(domain: RPRecordingErrorDomain, code: 10001,
                                      userInfo: [NSLocalizedDescriptionKey: "Mirroring stopped"])
                self?.finishBroadcastWithError(stopped)
            }
        }
    }

    // The app opens its end of the socket when the cast starts, which can be a
    // moment after the user picks this extension, so keep knocking.
    func openConnection() {
        let queue = DispatchQueue(label: "broadcast.connectTimer")
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now(), repeating: .milliseconds(100), leeway: .milliseconds(500))
        timer.setEventHandler { [weak self] in
            guard self?.clientConnection?.open() == true else { return }
            timer.cancel()
        }
        timer.resume()
    }
}
