/*
 * trace_session.h — vphone-traced launch-time session state machine.
 *
 * Blueprint v1.1.0 §6 states and §7-§9 pre-roll contract:
 *
 *     IDLE ──arm──▶ ARMED ──bind(pid)──▶ BOUND ──stop/exit──▶ IDLE
 *
 * While ARMED, every classified event is pushed into the ring (pre-roll).
 * BIND snapshots the sequencer, replays ring events matching the target
 * pid with sequence <= bind_sequence, then switches to live filtering
 * (sequence > bind_sequence). The monotonically increasing per-event
 * sequence makes the replay→live handoff gap- and overlap-free.
 *
 * The event source is pluggable so the launch state machine can be
 * exercised without a kernel session (blueprint §23: "synthetic events
 * first, do not integrate XNU kernel tracing yet").
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include "trace_event.h"
#include "trace_ring.h"

typedef struct vt_session vt_session;

typedef enum {
    VT_SESS_IDLE = 0,
    VT_SESS_ARMED,
    VT_SESS_BOUND,
    VT_SESS_STOPPING,
} vt_session_state;

/// Event source: fills one vt_event per call. Returns 1 when an event was
/// produced, 0 when temporarily starved, negative on fatal error.
typedef int (*vt_event_source_fn)(void *ctx, vt_event *out);

/// Session phase attribution for events delivered through the sink:
/// replayed (pre-bind, from ring) vs live (post-bind, streamed).
typedef enum {
    VT_PHASE_REPLAY = 0,
    VT_PHASE_LIVE = 1,
} vt_delivery_phase;

/// Streaming sink for replayed/live events (JSONL frame writer). `phase`
/// distinguishes pre-roll replay from live streaming (§9 duplicate guard).
typedef void (*vt_event_sink_fn)(void *ctx, const vt_event *event,
                                 vt_delivery_phase phase);

/// Control-connection sink for non-event frames (ready/bound/warn/exit).
typedef void (*vt_frame_sink_fn)(void *ctx, const char *json_line);

/// Session configuration.
typedef struct {
    uint64_t ring_capacity;     // pre-roll ring size in events
    vt_event_source_fn source;  // event producer (kernel pump or synthetic)
    void *source_ctx;
    vt_event_sink_fn sink;      // event consumer (stream writer)
    void *sink_ctx;
    vt_frame_sink_fn frame;     // control-frame consumer
    void *frame_ctx;
} vt_session_config;

/// Allocate and start a session in IDLE. The source pump runs on a dedicated
/// thread started here; the command handlers below are safe to call from the
/// connection thread.
int vt_session_create(vt_session **out, const vt_session_config *config);
/// ARM → ARMED. Starts buffering into the pre-roll ring. Returns 0 on
/// success, -EINVAL if not IDLE, -errno on pump-start failure. The `ready`
/// control frame is emitted by the pump once buffering is live, so the host
/// must not launch before it arrives.
int vt_session_arm(vt_session *s);

/// BIND → BOUND. Snapshots the sequence, replays matching ring events
/// (pid == target && sequence <= bind_sequence), then enables live
/// filtering. Returns 0 on success, -EINVAL if not ARMED, -ESRCH if the
/// replay produced no events (target unknown — caller decides policy).
int vt_session_bind(vt_session *s, uint32_t pid);

/// STOP — any state → STOPPING → IDLE. Tears the pump down; the ring is
/// retained for inspection until destroy.
void vt_session_stop(vt_session *s);

/// Current state (lock-free read of an atomic word).
vt_session_state vt_session_state_get(const vt_session *s);

/// Pre-roll overflow count (events overwritten while ARMED).
uint64_t vt_session_wrapped(const vt_session *s);

/// Total events observed (sequence counter value).
uint64_t vt_session_sequence(const vt_session *s);

void vt_session_destroy(vt_session *s);
