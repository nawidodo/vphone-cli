# Trace-Launch Pre-Roll Buffer (v1.1.0 §23 Deliverable)

Blueprint scope honored: **guest-side pre-roll buffering with a synthetic
event source — no XNU kernel tracing yet** (that is §24). The launch state
machine is implemented in C on the daemon side and fully exercised by a
native harness; the vsock wire-up lands with stage 7.

Status: `make sessiontest` — 8/8 honest exit-0 runs (pipefail, no pipeline
masking); `make hosttest` green; guest cross-build clean.

---

# 1. What was built

```
repo/scripts/vphone-traced/
├── trace_session.{c,h}     # IDLE→ARMED→BOUND→IDLE machine + pre-roll + replay
├── session_test.c          # native harness (synthetic source, pthreads)
├── trace_ring.{c,h}        # hardened: count/visit now lock (see §3)
└── Makefile                # `sessiontest` target wired; session in SRCS
```

The Stage-2 daemon (`vphone-traced.c`) is untouched — it still does raw
stdout tracing. `trace_session` is the stage-3 layer the daemon (and later
the vsock command loop) drives; both source kinds plug into the same
machine:

```c
vt_session_config cfg = {
    .ring_capacity = 65536,          // blueprint §7 default
    .source = source_fn,  .source_ctx = …,   // kernel pump OR synthetic
    .sink   = event_fn,   .sink_ctx   = …,   // JSONL stream writer
    .frame  = frame_fn,   .frame_ctx  = …,   // control frames (ready/bound/…)
};
vt_session_create(&s, &cfg);
vt_session_arm(s);            // IDLE → ARMED; READY frame emitted by pump
vt_session_bind(s, 843);      // ARMED → BOUND; bound frame, replay, live
vt_session_stop(s);           // any → IDLE; "stopped" frame
```

## 1.1 State machine (blueprint §6)

```
IDLE ──arm──▶ ARMED ──bind(pid)──▶ BOUND ──stop──▶ IDLE
  ▲            │ bind-before-arm rejected, double-arm rejected
  └────────────┴────── stop from any state
```

Guards are enforced: `arm` outside IDLE → `-EINVAL`; `bind` outside ARMED →
`-EINVAL`; `bind(pid==0)` → `-EINVAL`; `bind` with no matching buffered event
→ `-ESRCH` (caller decides policy — host reports launch failure per §16).

## 1.2 Pre-roll (blueprint §7/§8)

- Bounded ring, default **65536 events** (the §7 suggested size), power-of-two.
- While ARMED the pump pushes **every** event — no pid filter, per §8.
- Push past capacity overwrites the oldest and increments `wrapped`
  (detectable, never silent). The `bound` frame carries `"wrapped":N` so the
  host can print the §16 `[trace warning] pre-roll buffer wrapped` line.

## 1.3 Sequence boundary (blueprint §9)

The pump stamps a monotonically increasing per-event `sequence` under the
session lock. Dispatch state (`state`/`live_pid`/`bind_sequence`) is re-read
in the **same critical section that stamps the sequence** — after the source
call returns, not from a pre-source snapshot — so a BIND (or STOP) that
races the source can never mis-route an event into the pre-roll ring past
the frozen boundary (that would strand it: never replayed, never streamed).

BIND freezes the boundary **before** touching the ring:

1. `bind_sequence = sequence` (current watermark)
2. count matching ring events (`pid == target && sequence <= bind_sequence`)
3. emit `bound` frame (see §1.5 ordering)
4. publish `state = BOUND`, replay tagged events (oldest → newest)
5. live: pump streams only `pid == target && sequence > bind_sequence`,
   tagged `LIVE`

Replay and live are disjoint by construction; a pump event racing the bind
is strictly live-side. This is the no-duplicate/no-missing guarantee §15
Test 5 demands.

## 1.4 Delivery phases

Sink callbacks carry `vt_delivery_phase` (`VT_PHASE_REPLAY` /
`VT_PHASE_LIVE`), so consumers (and the harness) classify events exactly —
no positional or timestamp inference.

## 1.5 Frame ordering (blueprint §10.2)

`bind()` guarantees the wire order:

```
[bound frame] → [replayed events…] → [live events…]
```

Implementation: bound is emitted while the session lock is held and *before*
`state = BOUND` publishes; the pump cannot route a live event until it sees
BOUND, and it holds the same lock to do so. Replay begins immediately after
the bound frame, tagged `REPLAY`.

## 1.6 Control frames

- `{"type":"ready","pre_roll":true,"wrapped":0}` — emitted by the pump after
  ARM, once buffering is live. **The host must not call appLaunch before this
  arrives** (blueprint §3/§10.1).
- `{"type":"bound","pid":P,"replayed":N,"wrapped":W}` — N is the exact
  replay size, W the pre-roll wrap count (feeds the §16 host warning).
- `{"type":"stopped"}` — after stop; guest is back in IDLE.

## 1.7 Re-arm after stop (blueprint §6 relaunches)

`stop()` joins the pump and returns to IDLE; the session is **reusable**.
`arm()` on a stopped session respawns the pump thread and resets the
pre-roll ring (`vt_ring_reset(ring, session_sequence)`) — stale round-1
events can never leak into a round-2 replay. The global `sequence` counter
is deliberately **not** reset (monotonic across the process lifetime), so
event identities stay unique across relaunches; the ring's sequence mirror
is aligned to the session watermark at reset. `vt_ring_push` honors a
pre-stamped sequence (session pump) and only assigns ring-local sequences
for unstamped events. The harness proves reset **deterministically**: a capped source produces
exactly 8 events in round 1 (ring 256 — no wrap possible), then exactly 8
more in round 2. Without `vt_ring_reset`, round-2 replay would contain all
8 round-1 target events; with it, replay is exactly the 4 round-2 target
events (`replayed == 4`, every replayed sequence > round-1 stop watermark).
The assertion `r2_replay == 4` fails on any reset regression — no timing
dependence, no wrap-evection ambiguity.
- `{"type":"trace_drop"}` — surfaced when the source reports kernel drops.

---

# 2. Harness proof (session_test.c)

Synthetic source interleaves two pids (843 target / 900 other) round-robin;
the pump runs on its own thread; the collector records sink events (with
phase), control frames, all under a mutex.

| §23 test | What is asserted |
|---|---|
| Ring wraparound | 64-event ring overfilled (sequence watermark ≥ 300) → `wrapped > 0`; replay ≤ capacity; live events still flow after bind |
| PID filtering | every LIVE event is pid 843 after BIND; pid 900 never appears |
| Replay ordering | REPLAY-tagged events strictly ascending by sequence |
| Duplicate prevention | every LIVE event's sequence is strictly greater than the last REPLAY sequence; phases partition the stream exactly (`replay + live == count`) |
| Bound-frame fidelity | `bound.replayed` equals the delivered REPLAY count; `bound.pid` matches |
| Target exit handling | `stop()` → `stopped` frame → state IDLE |
| (state guards) | bind-before-arm, double-arm rejected; unknown pid → `-ESRCH` and nothing streams |

Run (honest exit-code form — pipelines must not mask assertion aborts):

```bash
cd repo/scripts/vphone-traced && set -o pipefail && make sessiontest
# session_test: all assertions passed   (exit 0; 8/8 consecutive runs)
```

## 2.1 Timing-tolerance note

The synthetic pump is unconstrained (it can produce >100k events/s), so at
bind time the ring may hold anywhere between a few and its full capacity.
The harness therefore asserts *phase-relative invariants* (replay ascending,
live strictly after replay, phase partition exact) and capacity bounds, not
exact counts — exact counts would test the scheduler, not the state machine.
Waits poll the session's own sequence watermark (`vt_session_sequence`),
never sink arrival, because ARMED-phase events do not reach the sink.

---

# 3. Ring hardening (advisory-driven)

The original ring let `count`/`visit` read `write_index` and slots without
locking while `push` mutated them — BIND replay could observe torn or
mid-overwrite records. Fixed:

- `vt_ring_count` takes the ring lock.
- `vt_ring_visit` copies each event out **under the ring lock** before
  invoking the callback (lock released around the callback). Callbacks
  observe stable snapshots and may take other locks (the session lock)
  without deadlock risk — lock order is strictly
  `session.lock → ring.lock`, never the reverse; no ring path takes the
  session lock.

---

# 4. What intentionally does NOT exist yet

| Piece | Lands in |
|---|---|
| vsock :1339 command loop + JSONL wire writer in the daemon (the `sink`/`frame` callbacks already emit the §10 frames verbatim) | §24 / stage 7 |
| Real XNU pump behind `vt_event_source_fn` (trace_ktrace.c already provides the records; the adapter is mechanical) | §24 fourth agent task |
| Host `VPhoneTraceClient` consuming `bound.replayed`/`bound.wrapped` and phase tags | stage 7/8 CLI task |

The host-side state machine (`sources/VPhoneTrace/`) already matches this
protocol: `VPhoneTraceResponse.bound(pid:wrapped:)` accepts the frames above
unchanged, and its `bind(pid:)` resolves on `bound` exactly when replay
completes guest-side.
