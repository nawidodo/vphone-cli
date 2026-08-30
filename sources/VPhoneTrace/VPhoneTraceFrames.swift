// VPhoneTraceFrames.swift — v1-protocol trace wire frames.
//
// Guest ⇄ host frames on the trace vsock channel are length-prefixed UTF-8
// JSON (same framing as vphoned's control channel). This file models the
// *payloads* only; transport (vsock/Unix socket/mock) is abstracted by
// VPhoneTraceTransport so the state machine is testable without a VM.

import Foundation

/// Messages the host can send to `vphone-traced`.
enum VPhoneTraceRequest: Equatable {
    /// ARM — start the kernel trace session and (optionally) pre-roll buffering.
    case arm(preRoll: Bool, captureMach: Bool)
    /// BIND — bind a resolved target PID; triggers pre-roll replay.
    case bind(pid: Int32)
    /// STOP — tear the session down; guest returns to IDLE.
    case stop
    /// PING — liveness check.
    case ping

    var payload: [String: Any] {
        switch self {
        case let .arm(preRoll, captureMach):
            return [
                "type": "arm",
                "pre_roll": preRoll,
                "mach": captureMach,
            ]
        case let .bind(pid):
            return ["type": "bind", "pid": Int(pid)]
        case .stop:
            return ["type": "stop"]
        case .ping:
            return ["type": "ping"]
        }
    }
}

/// Messages the guest can send to the host.
enum VPhoneTraceResponse: Equatable {
    /// READY — the requested session is armed (sent after `arm`).
    case ready(preRoll: Bool, wrapped: UInt64)
    /// BOUND — the pid was bound; replay begins after this frame.
    case bound(pid: Int32, wrapped: UInt64)
    /// OK — generic ack (stop/ping).
    case ok
    /// ERR — the guest rejected the request (bad state, ktrace failure, …).
    case err(message: String)
    /// A syscall (or metadata) event from the stream.
    case event(VPhoneTraceEvent)
    /// The bound target process exited; the stream is complete.
    case targetExit(pid: Int32)

    /// Decode a guest payload. Returns nil for unrecognized shapes so the
    /// caller can skip forward instead of tearing the channel down.
    static func decode(_ payload: [String: Any]) -> VPhoneTraceResponse? {
        let type = payload["type"] as? String ?? ""
        let wrapped = UInt64(payload["wrapped"] as? Int ?? 0)

        switch type {
        case "ready":
            return .ready(
                preRoll: payload["pre_roll"] as? Bool ?? false,
                wrapped: wrapped)
        case "bound":
            guard let pid = payload["pid"] as? Int, pid > 0 else { return nil }
            return .bound(pid: Int32(clamping: pid), wrapped: wrapped)
        case "ok":
            return .ok
        case "err":
            return .err(message: payload["message"] as? String ?? "unknown error")
        case "target_exit":
            guard let pid = payload["pid"] as? Int else { return nil }
            return .targetExit(pid: Int32(clamping: pid))
        case "syscall":
            guard let event = VPhoneTraceEvent(payload: payload) else { return nil }
            return .event(event)
        default:
            return nil
        }
    }
}

/// One decoded trace event. Stage-2 fidelity: raw fields only.
struct VPhoneTraceEvent: Equatable {
    var sequence: UInt64
    var timestamp: UInt64
    var pid: UInt32
    var tid: UInt64
    /// "bsd" | "mach" | "meta" | "other"
    var kind: String
    /// "entry" | "exit" | "none"
    var phase: String
    /// Syscall/trap number, or -1 for non-syscall records.
    var number: Int32
    var args: [UInt64]

    init?(payload: [String: Any]) {
        guard
            let sequence = payload["sequence"] as? Int,
            let timestamp = payload["timestamp"] as? Int,
            let kind = payload["kind"] as? String,
            let phase = payload["phase"] as? String
        else { return nil }

        self.sequence = UInt64(clamping: sequence)
        self.timestamp = UInt64(clamping: timestamp)
        self.pid = UInt32(clamping: payload["pid"] as? Int ?? 0)
        self.tid = UInt64(clamping: payload["tid"] as? Int ?? 0)
        self.kind = kind
        self.phase = phase
        self.number = Int32(clamping: payload["number"] as? Int ?? -1)
        self.args = (payload["args"] as? [Int] ?? []).map { UInt64(clamping: $0) }
    }

    /// Encode to the guest's JSONL shape (used by the mock and by tests).
    var payload: [String: Any] {
        [
            "type": "syscall",
            "sequence": Int(sequence),
            "timestamp": Int(timestamp),
            "pid": Int(pid),
            "tid": Int(tid),
            "kind": kind,
            "phase": phase,
            "number": Int(number),
            "args": args.map { Int(clamping: $0) },
        ]
    }
}

// MARK: - JSON bridging

enum VPhoneTraceCodec {
    static func encode(request: VPhoneTraceRequest) throws -> Data {
        var payload = request.payload
        payload["protocol"] = 1
        return try JSONSerialization.data(withJSONObject: payload)
    }

    static func decode(data: Data) throws -> VPhoneTraceResponse? {
        let object = try JSONSerialization.jsonObject(with: data)
        guard let payload = object as? [String: Any] else { return nil }
        return VPhoneTraceResponse.decode(payload)
    }
}
