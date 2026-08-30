// VPhoneTraceLaunch.swift — trace-launch orchestration.
//
// Owns the exact ordering the blueprint mandates (v1.1.0 §5):
//
//     try await tracer.arm()
//     try await tracer.waitUntilReady()      // (folded into arm() resolution)
//     let pid = try await launcher.launch()  // appLaunch — NEVER before READY
//     guard pid > 0 else { throw ... }
//     try await tracer.bind(pid: pid)
//
// `launching` is abstracted so tests can prove ordering with a mock and the
// CLI can later wire it to VPhoneControl.appLaunch (stage 7/8).

import Foundation

/// Abstraction over "launch the target and resolve its pid".
protocol VPhoneTraceLaunching: Sendable {
    func launch(bundleId: String) async throws -> Int32
}

enum VPhoneTraceLaunchError: Error, Equatable {
    case pidNotResolved(String)
}

/// The real launcher; calls VPhoneControl.appLaunch over the control channel.
/// Defined here against a protocol so VPhoneTrace stays decoupled from the
/// Virtualization-backed control object (and stays testable).
struct VPhoneControlTraceLauncher: VPhoneTraceLaunching {
    /// Mirrors VPhoneControl.appLaunch(bundleId:) -> Int (throws on failure).
    typealias AppLaunch = @Sendable (String) async throws -> Int

    private let appLaunch: AppLaunch

    /// - Parameter appLaunch: bind `VPhoneControl.appLaunch(bundleId:url:)`
    ///   at the call site: `{ try await control.appLaunch(bundleId: $0) }`.
    init(appLaunch: @escaping AppLaunch) {
        self.appLaunch = appLaunch
    }

    func launch(bundleId: String) async throws -> Int32 {
        let pid = try await appLaunch(bundleId)
        guard pid > 0 else {
            throw VPhoneTraceLaunchError.pidNotResolved(bundleId)
        }
        return Int32(clamping: pid)
    }
}

/// Run the full trace-launch sequence. Returns once the stream terminates
/// (target exit, transport close, or cancellation).
enum VPhoneTraceLaunch {
    /// Ordered sequence runner. `onEvent` receives each streamed event.
    ///
    /// - Throws: any arm/launch/bind failure. Launch failure guarantees BIND
    ///   was never attempted; READY failure guarantees launch never ran.
    static func run(
        bundleId: String,
        client: VPhoneTraceClient,
        launcher: VPhoneTraceLaunching,
        onEvent: @escaping @Sendable (VPhoneTraceEvent) -> Void = { _ in }
    ) async throws {
        // 1. ARM + WAIT READY (arm resolves on the guest's ready frame).
        try await client.arm(preRoll: true)

        // 2. LAUNCH — strictly after READY. A failure here leaves the session
        //    armed but never binds; stop() releases the guest.
        let pid: Int32
        do {
            pid = try await launcher.launch(bundleId: bundleId)
        } catch {
            try? await client.stop()
            throw error
        }

        // 3. BIND with exactly the pid appLaunch returned.
        try await client.bind(pid: pid)

        // 4. STREAM until target exit / disconnect.
        for try await event in client.stream() {
            onEvent(event)
        }
    }
}
