// VPhoneTraceTransport.swift — pluggable request/response byte channel.
//
// The real vsock transport lands with stage 7 (VPhoneControl owns the
// VZVirtioSocketDevice). Everything the state machine needs — framing,
// sequencing, ordering guarantees — is expressed against this protocol, which
// is also what the mock backend implements for the ordering proofs.

import Foundation

/// Errors surfaced by trace transports.
enum VPhoneTraceTransportError: Error, Equatable {
    /// The guest refused an operation (guest-reported `err` frame).
    case guestError(String)
    /// The channel closed (or the peer vanished) mid-operation.
    case disconnected
    /// A frame arrived that could not be decoded.
    case malformed(String)
}

/// A framed request/response byte channel to `vphone-traced`.
protocol VPhoneTraceTransport: Sendable {
    /// Send a request payload and await exactly one response payload.
    /// Implementations must be safe to call concurrently from one task at a
    /// time (the client serializes requests).
    func roundTrip(_ payload: Data) async throws -> Data

    /// Consume streamed frames (post-BIND event stream) until the peer
    /// closes or `stopStreaming()` is called. Each yielded Data is one
    /// encoded response payload.
    func stream() -> AsyncThrowingStream<Data, Error>

    /// Release the channel. Idempotent.
    func close() async
}

/// In-memory transport wiring a client to an in-process trace daemon
/// simulation. Used by the ordering tests; also a reference for the future
/// vsock transport's framing contract.
final class MockTraceTransport: VPhoneTraceTransport, @unchecked Sendable {
    /// Handler the test installs to play the guest role for round trips.
    var onRequest: ([String: Any]) async throws -> [String: Any]
    /// Handler the test installs to produce the post-BIND stream.
    var streamEvents: [VPhoneTraceEvent] = []

    private let queue = DispatchQueue(label: "com.vphone.trace.mock")
    private var continuation: AsyncThrowingStream<Data, Error>.Continuation?
    private var closed = false
    /// Frames pushed before stream() captured the continuation.
    private var pending: [Data] = []
    /// Every guest-facing request payload the client sent, in order.
    private(set) var receivedRequests: [[String: Any]] = []

    init(
        onRequest: @escaping ([String: Any]) async throws -> [String: Any] = { _ in
            ["type": "ok"]
        }
    ) {
        self.onRequest = onRequest
    }

    func roundTrip(_ payload: Data) async throws -> Data {
        let request = try JSONSerialization.jsonObject(with: payload)
        guard let dict = request as? [String: Any] else {
            throw VPhoneTraceTransportError.malformed("request is not an object")
        }
        queue.sync { receivedRequests.append(dict) }

        let response = try await onRequest(dict)
        return try JSONSerialization.data(withJSONObject: response)
    }

    func stream() -> AsyncThrowingStream<Data, Error> {
        // Capture, pending replay, and finish all happen inside ONE critical
        // section. AsyncThrowingStream.Continuation is thread-safe and its
        // yield/finish never call back into this transport, so holding
        // `queue` across them is safe and closes the race fully: a
        // concurrent finishStream() either runs before capture (→ `closed`
        // set, we finish at the end of this section) or blocks until this
        // section completes (→ it then sees the captured continuation and
        // finishes; the consumer observes frames first, finish last).
        // Buffered frames can never be dropped by an interleaved finish.
        AsyncThrowingStream { continuation in
            queue.sync {
                self.continuation = continuation
                for frame in pending { continuation.yield(frame) }
                pending.removeAll()
                if closed { continuation.finish() }
            }
        }
    }

    func close() async {
        let continuation: AsyncThrowingStream<Data, Error>.Continuation? =
            queue.sync {
                closed = true
                let c = self.continuation
                self.continuation = nil
                return c
            }
        continuation?.finish()
    }

    /// Test hook: push one encoded event into the stream.
    func yield(event: VPhoneTraceEvent) {
        guard let data = try? JSONSerialization.data(withJSONObject: event.payload) else {
            return
        }
        yield(raw: data)
    }

    /// Test hook: push one raw encoded frame into the stream. Safe to call
    /// before stream() — the frame is buffered and replayed on capture.
    /// Continuation calls happen while holding `queue` so ordering with a
    /// concurrent stream() capture is total.
    func yield(raw data: Data) {
        queue.sync {
            if let continuation {
                continuation.yield(data)
            } else {
                pending.append(data)
            }
        }
    }

    /// Test hook: finish the stream (simulates guest disconnect / target exit).
    /// Safe to call before stream() — closed state is honored on capture.
    func finishStream() {
        queue.sync {
            if let continuation {
                continuation.finish()
            } else {
                closed = true
            }
        }
    }
}
