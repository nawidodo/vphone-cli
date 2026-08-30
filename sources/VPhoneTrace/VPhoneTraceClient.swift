// VPhoneTraceClient.swift — host-side trace-launch state machine.
//
// Implements the v1.1.0 blueprint launch sequence with the strict ordering
// the blueprint mandates:
//
//     arm()  →  waitUntilReady()  →  appLaunch (caller)  →  bind(pid)  →  stream
//
// The client refuses to drift out of order: `bind` before READY, `arm` while
// already armed, etc. all throw. Sequence is caller-driven so the caller can
// insert the real `appLaunch` between READY and BIND — the exact step that
// must never happen before READY.
//
// State machine:
//
//     idle ──arm──▶ arming ──ready──▶ armed ──bind──▶ bound ──targetExit──▶ idle
//        ▲             │ fail          │ stop      │ stop          │ stop
//        └─────────────┴───────────────┴───────────┴───────────────┘

import Foundation

/// Client-side states mirroring the guest's (v1.1.0 §6).
enum VPhoneTraceClientState: Equatable {
    case idle
    /// `arm` sent, waiting for the guest's `ready`.
    case arming
    /// Guest confirmed the kernel trace session; pre-roll buffering.
    case armed
    /// A pid is bound; live (and replayed) events flow.
    case bound(pid: Int32)
}

/// Everything that can go wrong in the launch sequence.
enum VPhoneTraceError: Error, Equatable {
    case invalidState(expected: String, actual: VPhoneTraceClientState)
    case transportFailure(String)
    case guestRejected(String)
    case bindWithoutLaunch
    case pidInvalid(Int)
}

/// Observability hooks — the ordering tests assert on this journal rather
/// than on wall-clock behavior.
struct VPhoneTraceJournal: Equatable {
    enum Entry: Equatable {
        case armSent(preRoll: Bool)
        case readyReceived
        case bindSent(pid: Int32)
        case boundReceived(pid: Int32)
        case eventEmitted(VPhoneTraceEvent)
        case targetExitReceived(pid: Int32)
        case stopSent
    }

    var entries: [Entry] = []

    mutating func append(_ entry: Entry) { entries.append(entry) }

    /// Blueprint Test 1 helper: READY must precede any bind.
    var readyPrecedesBind: Bool {
        guard let ready = entries.firstIndex(where: { $0 == .readyReceived }),
              let bind = entries.firstIndex(where: {
                  if case .bindSent = $0 { return true }
                  return false
              })
        else { return true } // no bind yet → vacuously ordered
        return ready < bind
    }
}

final class VPhoneTraceClient: @unchecked Sendable {
    private let transport: VPhoneTraceTransport
    private let lock = NSLock()
    private var _state: VPhoneTraceClientState = .idle
    private var _journal = VPhoneTraceJournal()

    /// Pre-roll events collected between READY and BIND (replay source when
    /// the guest does not replay server-side; the mock guest streams them).
    private(set) var preRoll: [VPhoneTraceEvent] = []
    /// Set by the most recent ready/bind response.
    private(set) var lastWrappedCount: UInt64 = 0

    var state: VPhoneTraceClientState {
        lock.lock(); defer { lock.unlock() }
        return _state
    }

    var journal: VPhoneTraceJournal {
        lock.lock(); defer { lock.unlock() }
        return _journal
    }

    init(transport: VPhoneTraceTransport) {
        self.transport = transport
    }

    // MARK: - Sequence steps (each is one async step; never concurrent)

    /// ARM — must be the first operation. Resolves when the guest answers
    /// `ready` (the guest arms its kernel trace session before replying).
    func arm(preRoll: Bool = true, captureMach: Bool = false) async throws {
        try transition(from: .idle, to: .arming)
        journalAppend(.armSent(preRoll: preRoll))

        let response = try await roundTrip(.arm(preRoll: preRoll, captureMach: captureMach))
        switch response {
        case let .ready(preRoll, wrapped):
            applyReady(preRoll: preRoll, wrapped: wrapped)
        case let .err(message):
            setIdle()
            throw VPhoneTraceError.guestRejected(message)
        default:
            setIdle()
            throw VPhoneTraceError.transportFailure(
                "arm answered with unexpected \(response) ")
        }
    }

    /// BIND — only legal in `.armed`, and only with the pid the caller
    /// obtained from appLaunch. Resolves when the guest answers `bound`.
    func bind(pid: Int32) async throws {
        guard pid > 0 else { throw VPhoneTraceError.pidInvalid(Int(pid)) }
        let current = state
        guard current == .armed else {
            throw VPhoneTraceError.invalidState(expected: "armed", actual: current)
        }

        journalAppend(.bindSent(pid: pid))

        let response = try await roundTrip(.bind(pid: pid))
        switch response {
        case let .bound(boundPid, wrapped):
            guard boundPid == pid else {
                setIdle()
                throw VPhoneTraceError.guestRejected(
                    "guest bound pid \(boundPid), expected \(pid)")
            }
            applyBound(pid: pid, wrapped: wrapped)
        case let .err(message):
            setIdle()
            throw VPhoneTraceError.guestRejected(message)
        default:
            setIdle()
            throw VPhoneTraceError.transportFailure(
                "bind answered with unexpected \(response)")
        }
    }

    /// Live event stream (legal only while bound). Terminates when the guest
    /// reports `target_exit`, the transport closes, or `stop()` is issued.
    /// Events are also journaled so tests can prove the boundary.
    func stream() -> AsyncThrowingStream<VPhoneTraceEvent, Error> {
        AsyncThrowingStream { continuation in
            let task = Task {
                guard case .bound = self.state else {
                    continuation.finish(
                        throwing: VPhoneTraceError.invalidState(
                            expected: "bound", actual: self.state))
                    return
                }
                do {
                    for try await data in self.transport.stream() {
                        guard let response = try VPhoneTraceCodec.decode(data: data) else {
                            continue // skip unrecognized frames; never crash
                        }
                        switch response {
                        case let .event(event):
                            self.recordEvent(event)
                            continuation.yield(event)
                        case let .targetExit(pid):
                            self.finishWithTargetExit(pid: pid)
                            continuation.finish()
                            return
                        case .ok, .err, .ready, .bound:
                            continue
                        }
                    }
                    // Transport closed: treat as clean end of trace.
                    self.setIdle()
                    continuation.finish()
                } catch {
                    self.setIdle()
                    continuation.finish(throwing: error)
                }
            }
            continuation.onTermination = { _ in task.cancel() }
        }
    }

    /// STOP — legal from armed/bound; returns the guest to idle.
    func stop() async throws {
        let current = state
        var isBound = false
        if case .bound = current { isBound = true }
        guard current == .armed || isBound else { return }
        journalAppend(.stopSent)
        _ = try? await roundTrip(.stop)
        setIdle()
    }

    // MARK: - Sync lock scopes (NSLock is unavailable from async contexts)

    private func recordEvent(_ event: VPhoneTraceEvent) {
        lock.lock(); defer { lock.unlock() }
        _journal.append(.eventEmitted(event))
    }

    private func finishWithTargetExit(pid: Int32) {
        lock.lock(); defer { lock.unlock() }
        _journal.append(.targetExitReceived(pid: pid))
        _state = .idle
    }

    // MARK: - Internals

    private func journalAppend(_ entry: VPhoneTraceJournal.Entry) {
        lock.lock(); defer { lock.unlock() }
        _journal.append(entry)
    }

    private func applyReady(preRoll: Bool, wrapped: UInt64) {
        lock.lock(); defer { lock.unlock() }
        _state = .armed
        lastWrappedCount = wrapped
        _journal.append(.readyReceived)
    }

    private func applyBound(pid: Int32, wrapped: UInt64) {
        lock.lock(); defer { lock.unlock() }
        _state = .bound(pid: pid)
        lastWrappedCount = wrapped
        _journal.append(.boundReceived(pid: pid))
    }

    private func roundTrip(_ request: VPhoneTraceRequest) async throws -> VPhoneTraceResponse {
        let payload: Data
        do {
            payload = try VPhoneTraceCodec.encode(request: request)
        } catch {
            throw VPhoneTraceError.transportFailure("encode failed: \(error)")
        }
        let data: Data
        do {
            data = try await transport.roundTrip(payload)
        } catch let error as VPhoneTraceTransportError {
            setIdle()
            throw error
        }
        guard let response = try VPhoneTraceCodec.decode(data: data) else {
            setIdle()
            throw VPhoneTraceError.malformedResponse
        }
        return response
    }

    private func transition(from expected: VPhoneTraceClientState, to next: VPhoneTraceClientState) throws {
        lock.lock(); defer { lock.unlock() }
        guard _state == expected else {
            throw VPhoneTraceError.invalidState(expected: "\(expected)", actual: _state)
        }
        _state = next
    }

    private func setIdle() {
        lock.lock(); _state = .idle; lock.unlock()
    }
}

extension VPhoneTraceError {
    /// Localized decode failure marker (kept out of the enum so it stays
    /// Equatable without payload noise).
    static let malformedResponse = VPhoneTraceError.transportFailure("malformed response frame")
}

extension VPhoneTraceResponse: CustomStringConvertible {
    var description: String {
        switch self {
        case .ready: return "ready"
        case .bound: return "bound"
        case .ok: return "ok"
        case let .err(message): return "err(\(message))"
        case .event: return "event"
        case .targetExit: return "target_exit"
        }
    }
}
