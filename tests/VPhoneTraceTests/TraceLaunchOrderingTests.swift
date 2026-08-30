// TraceLaunchOrderingTests.swift — blueprint v1.1.0 §22 acceptance proofs.
//
// T1  READY ordering:  appLaunch never runs before the guest's READY.
// T2  PID flow:        BIND uses exactly the pid appLaunch returned.
// T3  Launch failure:  appLaunch throws → bind never sent, stop is sent.
// T4  READY failure:   arm/ready fails → appLaunch never runs.
// T5  State guards:    bind before arm / double arm are rejected client-side.
// T6  Stream end:      target_exit finishes the stream and resets to idle.

@testable import VPhoneTrace
import Foundation
import Testing

private let testEvent = VPhoneTraceEvent(
    payload: [
        "sequence": 1, "timestamp": 100, "pid": 843, "tid": 1127,
        "kind": "bsd", "phase": "entry", "number": 5, "args": [0x16f00000],
    ])!

@Suite("Trace launch ordering")
struct TraceLaunchOrderingTests {

    // MARK: helpers

    /// Build a client + mock transport whose guest answers ready/bound and
    /// whose "launch" is an observable, injectable closure.
    private func makeFixture(
        launchResult: Result<Int32, Error> = .success(843),
        readyDelay: UInt64 = 0
    ) -> (client: VPhoneTraceClient, mock: MockTraceTransport,
          launchOrder: LaunchOrder) {
        let order = LaunchOrder()
        let mock = MockTraceTransport { request in
            let type = request["type"] as? String ?? ""
            switch type {
            case "arm":
                if readyDelay > 0 {
                    try await Task.sleep(nanoseconds: readyDelay * 1_000_000)
                }
                order.append(.readySent)
                return ["type": "ready", "pre_roll": true, "wrapped": 0]
            case "bind":
                order.append(.bindReceived)
                return ["type": "bound", "pid": request["pid"] as? Int ?? 0,
                        "wrapped": 0]
            case "stop":
                order.append(.stopReceived)
                return ["type": "ok"]
            default:
                return ["type": "ok"]
            }
        }

        let client = VPhoneTraceClient(transport: mock)
        return (client, mock, order)
    }

    final class LaunchOrder: @unchecked Sendable {
        enum Entry: Equatable {
            case readySent, launchRan, bindReceived, stopReceived
        }

        private let lock = NSLock()
        private var entries: [Entry] = []
        func append(_ e: Entry) {
            lock.lock(); entries.append(e); lock.unlock()
        }
        var all: [Entry] {
            lock.lock(); defer { lock.unlock() }
            return entries
        }
    }

    private struct MockLauncher: VPhoneTraceLaunching {
        let body: @Sendable (String) async throws -> Int32
        func launch(bundleId: String) async throws -> Int32 {
            try await body(bundleId)
        }
    }

    // MARK: T1 — READY ordering

    @Test func readyAlwaysPrecedesLaunchAndBind() async throws {
        let (client, mock, order) = makeFixture()
        #expect(mock.streamEvents.isEmpty)

        // Drive the sequence the runner performs, asserting order at rest.
        try await client.arm()
        #expect(order.all == [.readySent], "guest ready before launch")
        #expect(client.state == .armed)

        let launcher = MockLauncher { _ in
            order.append(.launchRan)
            return 843
        }
        let pid = try await launcher.launch(bundleId: "com.example.target")
        #expect(pid == 843)

        try await client.bind(pid: pid)
        #expect(order.all == [.readySent, .launchRan, .bindReceived])
        #expect(client.state == .bound(pid: 843))

        // Journal-level invariant (blueprint Test 1 contract).
        #expect(client.journal.readyPrecedesBind)
    }

    @Test func readyDelayStillOrdersLaunchAfterReady() async throws {
        // Simulate a slow kernel-arm path: launch must still observe READY
        // first because arm() only resolves on the ready frame.
        let (client, mock, order) = makeFixture(readyDelay: 150)
        try await client.arm()
        let launcher = MockLauncher { _ in
            order.append(.launchRan)
            return 843
        }
        let pid = try await launcher.launch(bundleId: "com.example.target")
        try await client.bind(pid: pid)
        #expect(order.all == [.readySent, .launchRan, .bindReceived])
        #expect(client.journal.readyPrecedesBind)
    }

    // MARK: T2 — PID flow

    @Test func bindUsesPidReturnedByLaunch() async throws {
        let (client, mock, order) = makeFixture()
        try await client.arm()

        var requestedPid = 0
        mock.onRequest = { request in
            if request["type"] as? String == "bind" {
                requestedPid = request["pid"] as? Int ?? 0
                return ["type": "bound", "pid": requestedPid, "wrapped": 0]
            }
            return ["type": "ready", "pre_roll": true, "wrapped": 0]
        }

        let launchedPid: Int32 = 4242
        let launcher = MockLauncher { _ in launchedPid }
        let pid = try await launcher.launch(bundleId: "com.example.target")
        try await client.bind(pid: pid)

        #expect(requestedPid == Int(launchedPid))
        #expect(client.state == .bound(pid: launchedPid))
    }

    // MARK: T3 — Launch failure prevents BIND

    @Test func launchFailurePreventsBindAndStopsSession() async throws {
        let (client, _, order) = makeFixture()
        // run() performs the whole ARM → READY → LAUNCH → BIND sequence
        // itself; the client must start idle (the real blueprint flow).
        struct LaunchFailed: Error {}

        do {
            try await VPhoneTraceLaunch.run(
                bundleId: "com.example.target",
                client: client,
                launcher: MockLauncher { _ in throw LaunchFailed() })
            Issue.record("run should have thrown")
        } catch {
            // expected
        }

        #expect(!order.all.contains(.bindReceived), "bind must not run after launch failure")
        #expect(order.all.contains(.stopReceived), "session must be released")
        #expect(client.state == .idle)
    }

    // MARK: T4 — READY failure prevents launch

    @Test func readyFailurePreventsLaunch() async throws {
        let order = LaunchOrder()
        let mock = MockTraceTransport { _ in
            ["type": "err", "message": "kernel trace session could not be armed"]
        }
        let client = VPhoneTraceClient(transport: mock)

        do {
            try await VPhoneTraceLaunch.run(
                bundleId: "com.example.target",
                client: client,
                launcher: MockLauncher { _ in
                    order.append(.launchRan)
                    return 843
                })
            Issue.record("run should have thrown")
        } catch {
            // expected: guestRejected or transport failure
        }

        #expect(!order.all.contains(.launchRan), "launch must not run when READY fails")
        #expect(client.state == .idle)
    }

    // MARK: T5 — State guards

    @Test func bindBeforeArmIsRejected() async throws {
        let (client, _, _) = makeFixture()
        await #expect(throws: VPhoneTraceError.self) {
            try await client.bind(pid: 843)
        }
        #expect(client.state == .idle)
    }

    @Test func doubleArmIsRejected() async throws {
        let (client, _, _) = makeFixture()
        try await client.arm()
        await #expect(throws: VPhoneTraceError.self) {
            try await client.arm()
        }
        #expect(client.state == .armed)
    }

    @Test func bindWithInvalidPidIsRejected() async throws {
        let (client, _, _) = makeFixture()
        try await client.arm()
        await #expect(throws: VPhoneTraceError.pidInvalid(0).self) {
            try await client.bind(pid: 0)
        }
        #expect(client.state == .armed)
    }

    // MARK: T6 — Stream termination on target exit

    @Test func targetExitEndsStreamAndResetsState() async throws {
        let (client, mock, _) = makeFixture()

        // The mock plays the guest: after bind, stream events then exit.
        try await client.arm()
        try await client.bind(pid: 843)

        let stream = client.stream()
        var seen: [VPhoneTraceEvent] = []
        var terminated = false

        // Feed events + target_exit from the "guest" side, then finish.
        let feedTask = Task {
            mock.yield(event: testEvent)
            var e2 = testEvent
            e2.sequence = 2
            e2.number = 6
            e2.phase = "exit"
            mock.yield(event: e2)
            // target_exit frame
            let exit = try JSONSerialization.data(withJSONObject:
                ["type": "target_exit", "pid": 843])
            mock.yield(raw: exit)
            mock.finishStream()
        }

        for try await event in stream {
            seen.append(event)
        }
        try? await feedTask.value
        terminated = true

        #expect(terminated)
        #expect(seen.count == 2)
        #expect(seen.allSatisfy { $0.pid == 843 })
        #expect(client.state == .idle)
        #expect(client.journal.entries.contains(.targetExitReceived(pid: 843)))
    }

    // MARK: T7 — deterministic handoff barrier (pre-capture frames + finish)

    /// The advisory-hardening case: the feeder pushes all frames AND finishes
    /// *before* stream() captures the continuation. stream() must replay the
    /// buffered frames and then finish — deterministically, no frame dropped,
    /// no hang. With the atomic handoff this cannot lose a frame regardless
    /// of interleaving; running it pre-capture makes the ordering fully
    /// deterministic (capture happens when the consumer first iterates).
    @Test func framesBufferedBeforeStreamAreReplayedThenFinished() async throws {
        let (client, mock, _) = makeFixture()
        try await client.arm()
        try await client.bind(pid: 843)

        // Feed everything BEFORE creating the stream: all frames land in
        // pending, and closed is set. No concurrent feeder at all.
        mock.yield(event: testEvent)
        var e2 = testEvent
        e2.sequence = 2
        e2.number = 6
        e2.phase = "exit"
        mock.yield(event: e2)
        let exit = try JSONSerialization.data(withJSONObject:
            ["type": "target_exit", "pid": 843])
        mock.yield(raw: exit)
        mock.finishStream()

        // Now capture + consume. Capture must replay [e1, e2, exit-frame]
        // under one queue section, then finish. The client decodes exit-frame
        // as targetExit → state idle; the folded finish ends the iteration.
        var seen: [VPhoneTraceEvent] = []
        for try await event in client.stream() {
            seen.append(event)
        }

        #expect(seen.count == 2)
        #expect(seen.map(\.sequence) == [1, 2])
        #expect(client.state == .idle)
    }
}
