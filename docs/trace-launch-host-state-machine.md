# Trace-Launch Host State Machine (v1.1.0 §22 Deliverable)

Blueprint scope honored: **host-side trace-launch state machine with a mock
trace backend only — no kernel tracing, no VM integration.** The four §22
proof obligations are automated in `tests/VPhoneTraceTests/`.

Status: `swift test --filter TraceLaunch` — 10/10 green (5 consecutive runs);
no VM required.

---

# 1. What was built

```
repo/sources/VPhoneTrace/                 # new library target (AppKit-free)
├── VPhoneTraceFrames.swift               # request/response payloads, event model, JSON codec
├── VPhoneTraceTransport.swift            # transport protocol + MockTraceTransport
├── VPhoneTraceClient.swift               # the state machine (idle→arming→armed→bound→idle)
└── VPhoneTraceLaunch.swift               # ordered runner: arm→ready→launch→bind→stream

repo/tests/VPhoneTraceTests/
└── TraceLaunchOrderingTests.swift        # T1–T7 proofs (swift-testing)

repo/Package.swift                        # VPhoneTrace target + VPhoneTraceTests wired
```

`vphone-cli` depends on `VPhoneTrace` (no reverse dependency; `VPhoneControl`
is untouched per blueprint §11).

## 1.1 API surface

```swift
let client = VPhoneTraceClient(transport: any VPhoneTraceTransport)

try await client.arm(preRoll: Bool = true, captureMach: Bool = false) // idle → armed (resolves on guest READY)
let pid: Int32 = try await launcher.launch(bundleId:)                 // caller-owned: appLaunch
try await client.bind(pid: pid)                                       // armed → bound(pid)
for try await event in client.stream() { … }                          // until target_exit/disconnect
try await client.stop()                                               // armed|bound → idle

// orchestration of the whole sequence with ordering enforced:
try await VPhoneTraceLaunch.run(bundleId:client:launcher:onEvent:)
```

State guards (client-side, fail fast without touching the transport):

- `arm` only from `.idle` — re-arm throws `invalidState`.
- `bind` only from `.armed` and `pid > 0` — throws `invalidState` /
  `pidInvalid`.
- Guest pid echo mismatch on `bound` throws `guestRejected`.
- `stream()` requires `.bound`; unrecognized frames are skipped, never fatal.

Every state change is journaled (`VPhoneTraceJournal`) — `armSent`,
`readyReceived`, `bindSent(pid)`, `boundReceived(pid)`, `eventEmitted`,
`targetExitReceived`, `stopSent` — which is what makes the ordering proofs
deterministic assertions rather than timing heuristics.

## 1.2 Transport abstraction

`VPhoneTraceTransport` is one method + one stream:

```swift
func roundTrip(_ payload: Data) async throws -> Data   // arm/bind/stop/ping
func stream() -> AsyncThrowingStream<Data, Error>      // post-bind event frames
func close() async
```

`MockTraceTransport` implements the guest side in-process (request handler
closure + stream feed hooks) and records every request in order — the
`readyDelay` fixture simulates a slow kernel-arm path without threads.
The real vsock transport (stage 7) only has to satisfy this protocol.

## 1.3 Launch abstraction

`VPhoneTraceLaunching.launch(bundleId:) -> Int32` is the seam where stage 7/8
will bind `VPhoneControl.appLaunch(bundleId:url:)`:

```swift
VPhoneControlTraceLauncher { bundleId in
    try await control.appLaunch(bundleId: bundleId)   // returns Int pid
}
```

Launch failure inside `VPhoneTraceLaunch.run` triggers `client.stop()` so the
guest session is released and the failure is surfaced — never silently
swallowed (blueprint §16).

---

# 2. Ordering proofs (v1.1.0 §22)

| §22 requirement | Test | Mechanism |
|---|---|---|
| appLaunch is never called before READY | `readyAlwaysPrecedesLaunchAndBind`, `readyDelayStillOrdersLaunchAfterReady` | Mock guest appends `readySent` before answering; launcher appends `launchRan`; suite asserts exact order `[.readySent, .launchRan, .bindReceived]` and journal invariant `readyPrecedesBind`. The 150 ms delay variant proves ordering survives a slow arm, not just sequential luck. |
| BIND uses the PID returned by appLaunch | `bindUsesPidReturnedByLaunch` | Mock guest records the `pid` of the bind frame; test asserts `requestedPid == launchedPid` and final state `.bound(pid:)`. |
| Launch failure prevents BIND | `launchFailurePreventsBindAndStopsSession` | Throwing launcher → run() must throw, never send bind, send stop, and reset to idle. |
| READY failure prevents app launch | `readyFailurePreventsLaunch` | Guest answers `err` to arm → run() throws before the launcher body runs (which would have recorded `launchRan`). |

Additional guards beyond §22 (blueprint §6 state machine + §16 failure
handling):

- `bindBeforeArmIsRejected`, `doubleArmIsRejected`,
  `bindWithInvalidPidIsRejected` — client-side state machine enforcement.
- `targetExitEndsStreamAndResetsState` — two streamed events then
  `target_exit`; asserts the stream terminates, the journal records the exit,
  and the state returns to idle (blueprint §13: trace ends when the bound PID
  exits).
- `framesBufferedBeforeStreamAreReplayedThenFinished` (T7) — deterministic
  handoff barrier: all frames plus finish are pushed *before* `stream()`
  captures; the mock's atomic capture-replay-finish (single critical section)
  must replay both events and fold the finish. Locks in the no-lost-frame
  guarantee against the pre-capture interleaving class.

Run:

```bash
cd repo && swift test --filter TraceLaunch
# 􀢄 Test run with 10 tests in 1 suite passed
```

Full-package `swift test` also runs the pre-existing suites; the 21 failures
in `FirmwarePatcherTests` / `BundleOpsTests` (glob matching, export/import)
are **pre-existing on unmodified `main`** (verified via `git stash`) and
unrelated to this change — my delta adds 10 passing tests to the 188-test
baseline (198 total after).

---

# 3. Concurrency notes (Swift 6 strict concurrency)

- `VPhoneTraceClient` uses `NSLock`-guarded state with **synchronous helper
  methods** (`journalAppend`, `applyReady`, `applyBound`, `recordEvent`,
  `finishWithTargetExit`, `setIdle`) because `NSLock.lock/unlock` is
  unavailable inside async contexts — the first build caught three such
  violations, all refactored into sync scopes.
- `MockTraceTransport` serializes **all** continuation/pending/closed state
  on one `DispatchQueue`, and executes continuation calls *while holding
  it* (capture, pending replay, and finish share one critical section).
  `AsyncThrowingStream.Continuation` is thread-safe and its `yield`/`finish`
  never call back into the transport, so holding the queue across them is
  safe and makes the ordering with a concurrent `stream()` capture total.
  Frames pushed before capture are buffered and replayed in the same
  section; `finishStream()` before capture sets `closed` and is folded into
  the same section — a finish can never land between capture and replay to
  drop a frame. T6 covers the concurrent-feeder path; T7 is the
  deterministic pre-capture barrier.
- The stream consumes on the caller's task; `onTermination` cancels the
  reader task on early termination.

---

# 4. What intentionally does NOT exist yet

| Piece | Lands in |
|---|---|
| Real vsock transport (guest port 1339) | §22 stage 7 (with `vphone-traced` vsock listener) |
| `VPhoneControl.appLaunch` binding + `TraceCommand` CLI | §22 stage 8 / host CLI task |
| Kernel trace behind the mock | v1.1.0 §24 fourth agent task (C side already has `#ifdef`-free raw reader from stage 2; the guest daemon grows a `bind`/replay layer) |
| Pre-roll replay driven by the guest | third agent task (§23) — the client's `preRoll` buffer + `sequence` fields are already in place for it |

The mock's wire shapes are byte-for-byte the frames documented in
`docs/trace-launch-architecture-review.md` §3/§10, so stage 7's transport is a
drop-in swap, not a protocol change.
