/*
 * trace_session.c — launch-time session state machine with pre-roll ring.
 *
 * Threading model:
 *   - pump thread: pulls events from the source, stamps the sequence, and
 *     either buffers (ARMED) or streams (BOUND). Owns `sequence` writes.
 *   - control thread (vsock/CLI): calls arm/bind/stop.
 *
 * Lock ordering (deadlock freedom):
 *   session.lock -> ring.lock  (bind holds the session lock, then replays;
 *                               the pump holds the session lock, then pushes)
 *   The ring's own visit lock is acquired *inside* both, never the reverse —
 *   no ring path ever takes the session lock.
 *
 * The bind path (blueprint v1.1.0 §8/§9):
 *   1. session.lock held
 *   2. bind_sequence = sequence (current pump watermark)
 *   3. walk ring oldest→newest; emit events with pid==target &&
 *      sequence<=bind_sequence  (ring.visit copies each event out under the
 *      ring lock, so records are stable even though the pump is only paused
 *      by the session lock we hold)
 *   4. live_pid = target; state = BOUND
 *   5. unlock
 * From that point the pump streams only pid==target && sequence>
 * bind_sequence events. Replay and live are disjoint by construction (§9);
 * pump events racing the bind are strictly live-side.
 */

#include "trace_session.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct vt_session {
    vt_session_config cfg;

    pthread_mutex_t lock;
    pthread_cond_t pump_cond;
    pthread_t pump_thread;
    int pump_running;

    vt_session_state state;     // guarded by lock
    uint32_t live_pid;          // guarded by lock; 0 = filter nothing
    uint64_t bind_sequence;     // guarded by lock; replay/live boundary
    uint64_t sequence;          // guarded by lock; monotonic counter
    uint64_t dropped_markers;   // TRACE_LOST_EVENTS count seen
    int stop_requested;         // guarded by lock
    int ready_sent;             // guarded by lock; READY emitted once

    vt_ring ring;
};

// ── emitters ─────────────────────────────────────────────────────────────

static void session_emit_frame(vt_session *s, const char *line) {
    if (s->cfg.frame) s->cfg.frame(s->cfg.frame_ctx, line);
}

static void session_emit_event(vt_session *s, const vt_event *ev,
                               vt_delivery_phase phase) {
    if (s->cfg.sink) s->cfg.sink(s->cfg.sink_ctx, ev, phase);
}

// ── pump ─────────────────────────────────────────────────────────────────

static void *pump_main(void *arg) {
    vt_session *s = arg;

    while (1) {
        pthread_mutex_lock(&s->lock);
        while (!s->stop_requested && s->state == VT_SESS_IDLE) {
            pthread_cond_wait(&s->pump_cond, &s->lock);
        }
        if (s->stop_requested) {
            pthread_mutex_unlock(&s->lock);
            break;
        }

        if (!s->ready_sent) {
            // Announce readiness once buffering is live (blueprint §3: the
            // host must not launch before READY arrives).
            s->ready_sent = 1;
            pthread_mutex_unlock(&s->lock);
            session_emit_frame(s,
                "{\"type\":\"ready\",\"pre_roll\":true,\"wrapped\":0}");
            continue;
        }
        // NOTE: only the wait/READY decision uses pre-source state. After
        // the source returns we re-acquire the lock and re-read state/
        // live_pid/bind_sequence: a BIND (or STOP) may have happened while
        // the source was running, and dispatching on the stale snapshot
        // would push a post-boundary event into the pre-roll ring where it
        // is never replayed nor live-streamed (a §9 handoff gap).
        pthread_mutex_unlock(&s->lock);

        vt_event ev;
        int got = s->cfg.source(s->cfg.source_ctx, &ev);

        pthread_mutex_lock(&s->lock);
        if (got <= 0) {
            int stop = s->stop_requested;
            pthread_mutex_unlock(&s->lock);
            if (got < 0 || stop) break; // fatal source error or shutdown
            continue;                   // starved this tick
        }
        if (s->stop_requested) {
            pthread_mutex_unlock(&s->lock);
            break;
        }

        // Fresh dispatch state — post-source, same critical section as the
        // sequence stamp so watermark and routing decision are consistent.
        vt_session_state state = s->state;
        uint32_t live_pid = s->live_pid;
        uint64_t bind_sequence = s->bind_sequence;

        s->sequence++;
        ev.sequence = s->sequence;
        if (ev.debugid == VT_TRACE_LOST_EVENTS) {
            s->dropped_markers++;
        }

        switch (state) {
        case VT_SESS_ARMED:
            // Pre-roll: buffer everything, no pid filter yet (§8).
            vt_ring_push(&s->ring, &ev);
            break;
        case VT_SESS_BOUND:
            // Live: only the bound pid, strictly after the replay boundary
            // (§9: sequence > bind_sequence). Because state/watermark are
            // read in the same critical section that stamps the sequence,
            // this test is exact — no event can fall into the ring after
            // the boundary was frozen.
            if (ev.debugid == VT_TRACE_LOST_EVENTS) {
                pthread_mutex_unlock(&s->lock);
                session_emit_frame(s, "{\"type\":\"trace_drop\"}");
                pthread_mutex_lock(&s->lock);
            } else if (ev.pid == live_pid && ev.sequence > bind_sequence) {
                pthread_mutex_unlock(&s->lock);
                session_emit_event(s, &ev, VT_PHASE_LIVE);
                pthread_mutex_lock(&s->lock);
            }
            break;
        default:
            // IDLE/STOPPING: discard.
            break;
        }
        pthread_mutex_unlock(&s->lock);
    }
    return NULL;
}

// ── lifecycle ────────────────────────────────────────────────────────────

int vt_session_create(vt_session **out, const vt_session_config *config) {
    if (!out || !config || !config->source) return -EINVAL;

    vt_session *s = calloc(1, sizeof(*s));
    if (!s) return -ENOMEM;
    s->cfg = *config;

    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->pump_cond, NULL);
    s->state = VT_SESS_IDLE;

    uint64_t cap = config->ring_capacity ? config->ring_capacity : 65536;
    int rc = vt_ring_init(&s->ring, cap);
    if (rc != 0) {
        pthread_cond_destroy(&s->pump_cond);
        pthread_mutex_destroy(&s->lock);
        free(s);
        return rc;
    }

    s->pump_running = 1;
    if (pthread_create(&s->pump_thread, NULL, pump_main, s) != 0) {
        s->pump_running = 0;
        vt_ring_destroy(&s->ring);
        pthread_cond_destroy(&s->pump_cond);
        pthread_mutex_destroy(&s->lock);
        free(s);
        return -errno;
    }

    *out = s;
    return 0;
}

int vt_session_arm(vt_session *s) {
    if (!s) return -EINVAL;
    int rc = 0;
    pthread_mutex_lock(&s->lock);
    if (s->state != VT_SESS_IDLE) {
        pthread_mutex_unlock(&s->lock);
        return -EINVAL;
    }

    // Re-arm after stop(): the pump thread exited on shutdown, so respawn
    // it. From the caller's view the session is reusable IDLE → ARMED → …
    // → IDLE for its whole lifetime (blueprint §6: relaunches are in scope).
    if (!s->pump_running) {
        s->stop_requested = 0;
        s->ready_sent = 0;
        s->pump_running = 1;
        if (pthread_create(&s->pump_thread, NULL, pump_main, s) != 0) {
            s->pump_running = 0;
            rc = -errno;
            pthread_mutex_unlock(&s->lock);
            return rc;
        }
    }
    s->state = VT_SESS_ARMED;
    s->ready_sent = 0;
    // Fresh pre-roll per launch: stale round-1 events must never leak into
    // a round-2 replay. Reset under the ring's own lock; the global session
    // sequence is NOT reset (monotonic across the process lifetime), so the
    // ring's last-assigned-sequence mirror is aligned to it.
    vt_ring_reset(&s->ring, s->sequence);
    pthread_cond_broadcast(&s->pump_cond);
    pthread_mutex_unlock(&s->lock);
    return rc;
}

// Snapshot callback: counts matching events without emitting. Runs under
// the session lock; the pump cannot route anything mid-pass.
struct count_ctx {
    uint32_t pid;
    uint64_t bind_sequence;
    long count;
};

static void count_visit(const vt_event *e, void *ctx) {
    struct count_ctx *c = ctx;
    if (e->pid == c->pid && e->sequence <= c->bind_sequence) c->count++;
}

// Replay callback: emits matching events with REPLAY phase attribution.
struct replay_ctx {
    vt_session *s;
    uint32_t pid;
    uint64_t bind_sequence;
    long count;
};

static void replay_visit(const vt_event *e, void *ctx) {
    struct replay_ctx *r = ctx;
    if (e->pid == r->pid && e->sequence <= r->bind_sequence) {
        session_emit_event(r->s, e, VT_PHASE_REPLAY);
        r->count++;
    }
}

int vt_session_bind(vt_session *s, uint32_t pid) {
    if (!s || pid == 0) return -EINVAL;

    pthread_mutex_lock(&s->lock);
    if (s->state != VT_SESS_ARMED) {
        pthread_mutex_unlock(&s->lock);
        return -EINVAL;
    }

    // Freeze the boundary BEFORE touching the ring (§9). Pump events that
    // land later carry sequence > bind_sequence and stream as live.
    s->bind_sequence = s->sequence;

    // Phase 1 — count matching events (no emission). The ring cannot change
    // while we hold the session lock, so this snapshot is authoritative.
    struct count_ctx cctx = { pid, s->bind_sequence, 0 };
    vt_ring_visit(&s->ring, count_visit, &cctx);
    long count = cctx.count;
    uint64_t wrapped = s->ring.wrapped;

    // Phase 2 — BOUND first (blueprint §10.2: "then trace events begin";
    // the host must receive bound before any replayed/live event).
    char frame[128];
    snprintf(frame, sizeof(frame),
             "{\"type\":\"bound\",\"pid\":%u,\"replayed\":%ld,\"wrapped\":%llu}",
             pid, count, (unsigned long long)wrapped);
    session_emit_frame(s, frame);

    // Phase 3 — publish BOUND and replay. From the moment `state` becomes
    // BOUND the pump routes live events (VT_PHASE_LIVE), and every live
    // event is emitted after this unlock, so the stream order is exactly:
    //   [bound frame] → [replay…] → [live…]
    s->state = VT_SESS_BOUND;
    s->live_pid = pid;

    struct replay_ctx rctx = { s, pid, s->bind_sequence, 0 };
    vt_ring_visit(&s->ring, replay_visit, &rctx);
    pthread_mutex_unlock(&s->lock);

    return count > 0 ? 0 : -ESRCH;
}

void vt_session_stop(vt_session *s) {
    if (!s) return;
    pthread_mutex_lock(&s->lock);
    s->state = VT_SESS_STOPPING;
    s->stop_requested = 1;
    s->live_pid = 0;
    pthread_cond_broadcast(&s->pump_cond);
    pthread_mutex_unlock(&s->lock);
    pthread_join(s->pump_thread, NULL);
    s->pump_running = 0;
    s->state = VT_SESS_IDLE;
    session_emit_frame(s, "{\"type\":\"stopped\"}");
}

vt_session_state vt_session_state_get(const vt_session *s) {
    if (!s) return VT_SESS_IDLE;
    pthread_mutex_lock(&((vt_session *)s)->lock);
    vt_session_state st = s->state;
    pthread_mutex_unlock(&((vt_session *)s)->lock);
    return st;
}

uint64_t vt_session_wrapped(const vt_session *s) {
    if (!s) return 0;
    pthread_mutex_lock(&((vt_session *)s)->lock);
    uint64_t w = s->ring.wrapped;
    pthread_mutex_unlock(&((vt_session *)s)->lock);
    return w;
}

uint64_t vt_session_sequence(const vt_session *s) {
    if (!s) return 0;
    pthread_mutex_lock(&((vt_session *)s)->lock);
    uint64_t q = s->sequence;
    pthread_mutex_unlock(&((vt_session *)s)->lock);
    return q;
}

void vt_session_destroy(vt_session *s) {
    if (!s) return;
    if (s->pump_running) vt_session_stop(s);
    vt_ring_destroy(&s->ring);
    pthread_cond_destroy(&s->pump_cond);
    pthread_mutex_destroy(&s->lock);
    free(s);
}
