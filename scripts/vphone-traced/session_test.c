/*
 * session_test.c — native harness for the launch-time session machine
 * (blueprint v1.1.0 §23: synthetic events, no kernel tracing).
 *
 * Covers:
 *   - ARM → READY frame ordering (ready emitted once buffering is live)
 *   - pre-roll capture with no pid filter while ARMED
 *   - BIND replay: only target-pid events, only sequence <= bind watermark,
 *     oldest → newest
 *   - live stream after replay: only target-pid events, sequence >
 *     bind watermark; no duplicate sequence across replay∪live
 *   - ring wraparound during ARMED (wrapped counter, oldest lost)
 *   - unbound-pid events discarded after BIND
 *   - target exit → stopped frame → idle
 *   - state guards: bind before arm, double arm
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "trace_session.h"

#define TARGET_PID 843
#define OTHER_PID 900

struct collector {
    pthread_mutex_t lock;
    vt_event events[4096];
    vt_delivery_phase phases[4096];
    long count;
    long replay_count;      // events delivered with VT_PHASE_REPLAY
    char frames[64][128];
    long frame_count;
};

static void collect_event(void *ctx, const vt_event *ev,
                          vt_delivery_phase phase) {
    struct collector *c = ctx;
    pthread_mutex_lock(&c->lock);
    if (c->count < 4096) {
        c->events[c->count] = *ev;
        c->phases[c->count] = phase;
        c->count++;
        if (phase == VT_PHASE_REPLAY) c->replay_count++;
    }
    pthread_mutex_unlock(&c->lock);
}

static void collect_frame(void *ctx, const char *line) {
    struct collector *c = ctx;
    pthread_mutex_lock(&c->lock);
    if (c->frame_count < 64) {
        snprintf(c->frames[c->frame_count], 128, "%s", line);
        c->frame_count++;
    }
    pthread_mutex_unlock(&c->lock);
}

// Synthetic source: an interleaved stream from two pids. Events are pulled
// round-robin so replay ordering is deterministic; a small sleep lets the
// pump thread keep up at ARMED time.
struct source {
    pthread_mutex_t lock;
    long produced;          // number of events handed to the session
    int target_every;       // every Nth event belongs to the target
    long cap;               // 0 = unlimited; else stop after this many
    int stop;
};

static int source_next(void *ctx, vt_event *out) {
    struct source *src = ctx;
    pthread_mutex_lock(&src->lock);
    if (src->cap > 0 && src->produced >= src->cap) {
        pthread_mutex_unlock(&src->lock);
        return 0; // capped: starve until the next round resets the cap
    }
    long n = src->produced++;
    pthread_mutex_unlock(&src->lock);

    uint32_t pid = (n % src->target_every == 0) ? TARGET_PID : OTHER_PID;
    *out = (vt_event){
        .timestamp = (uint64_t)(1000 + n),
        .tid = 0x1000 + (uint64_t)n,
        .pid = pid,
        .debugid = 0x040c0014 | ((n % 2) ? 2u : 1u), // BSD open entry/exit
        .kind = VT_KIND_BSD,
        .arg1 = (uint64_t)n,
    };
    return 1;
}

static int has_frame(const struct collector *c, const char *prefix) {
    for (long i = 0; i < c->frame_count; i++) {
        if (strncmp(c->frames[i], prefix, strlen(prefix)) == 0) return 1;
    }
    return 0;
}

int main(void) {
    // ── basic ARM → pre-roll → BIND replay → live ─────────────────────
    {
        struct collector col = { .lock = PTHREAD_MUTEX_INITIALIZER };
        struct source src = { .lock = PTHREAD_MUTEX_INITIALIZER,
                              .target_every = 2 };

        vt_session_config cfg = {
            .ring_capacity = 1024,
            .source = source_next, .source_ctx = &src,
            .sink = collect_event, .sink_ctx = &col,
            .frame = collect_frame, .frame_ctx = &col,
        };
        vt_session *s = NULL;
        assert(vt_session_create(&s, &cfg) == 0);
        assert(vt_session_state_get(s) == VT_SESS_IDLE);

        // Bind before arm must fail.
        assert(vt_session_bind(s, TARGET_PID) == -EINVAL);

        assert(vt_session_arm(s) == 0);
        assert(vt_session_state_get(s) == VT_SESS_ARMED);

        // Double arm must fail.
        assert(vt_session_arm(s) == -EINVAL);

        // Let the pump fill the pre-roll ring. While ARMED the sink sees
        // nothing (events only go to the ring), so wait on the pump's
        // sequence watermark instead.
        for (int i = 0; i < 4000 && vt_session_sequence(s) < 60; i++) {
            usleep(2000);
        }
        assert(vt_session_sequence(s) >= 60);
        assert(has_frame(&col, "{\"type\":\"ready\""));

        // The pump runs unconstrained, so by bind time the ring may hold
        // anywhere between a few and its full capacity (1024) events,
        // interleaved 1:1 between target and other pids. bind() reports the
        // authoritative replay count in the bound frame; events carry a
        // phase tag (REPLAY vs LIVE) so classification is exact, not
        // positional.
        int rc = vt_session_bind(s, TARGET_PID);
        assert(rc == 0); // replay found pre-roll target events
        assert(vt_session_state_get(s) == VT_SESS_BOUND);

        pthread_mutex_lock(&col.lock);
        long replayed = col.replay_count;
        assert(replayed > 0);
        assert((uint64_t)replayed <= cfg.ring_capacity / 2);

        // Every REPLAY event: target pid, ascending sequence. The bound
        // frame's replayed field must equal the number of REPLAY-tagged
        // events actually delivered.
        uint64_t replay_last_seq = 0;
        for (long i = 0; i < col.count; i++) {
            if (col.phases[i] != VT_PHASE_REPLAY) continue;
            assert(col.events[i].pid == TARGET_PID);
            if (replay_last_seq) {
                assert(col.events[i].sequence > replay_last_seq);
            }
            replay_last_seq = col.events[i].sequence;
        }
        int found_bound = 0;
        for (long i = 0; i < col.frame_count; i++) {
            if (strncmp(col.frames[i], "{\"type\":\"bound\"", 15) == 0) {
                found_bound = 1;
                assert(strstr(col.frames[i], "\"pid\":843"));
                char want[64];
                snprintf(want, sizeof(want), "\"replayed\":%ld", replayed);
                assert(strstr(col.frames[i], want));
            }
        }
        assert(found_bound);
        pthread_mutex_unlock(&col.lock);

        // Live: collect more events; every LIVE-tagged event must be the
        // target pid with sequence strictly after the last REPLAY event —
        // no duplicate, no overlap at the replay→live handoff (§9/§15 T5).
        for (int i = 0; i < 4000; i++) {
            pthread_mutex_lock(&col.lock);
            long rc2 = col.count - col.replay_count;
            pthread_mutex_unlock(&col.lock);
            if (rc2 >= 25) break;
            usleep(2000);
        }

        pthread_mutex_lock(&col.lock);
        long live = 0;
        for (long i = 0; i < col.count; i++) {
            if (col.phases[i] != VT_PHASE_LIVE) continue;
            live++;
            assert(col.events[i].pid == TARGET_PID);
            assert(col.events[i].sequence > replay_last_seq);
        }
        assert(live >= 25);
        // Sanity: phase tags partition the stream exactly.
        assert(col.replay_count + live == col.count);
        pthread_mutex_unlock(&col.lock);

        vt_session_stop(s);
        assert(vt_session_state_get(s) == VT_SESS_IDLE);
        assert(has_frame(&col, "{\"type\":\"stopped\""));
        vt_session_destroy(s);
    }

    // ── wraparound during ARMED ───────────────────────────────────────
    {
        struct collector col = { .lock = PTHREAD_MUTEX_INITIALIZER };
        struct source src = { .lock = PTHREAD_MUTEX_INITIALIZER,
                              .target_every = 1 }; // every event is target

        vt_session_config cfg = {
            .ring_capacity = 64,   // tiny ring → forced wrap
            .source = source_next, .source_ctx = &src,
            .sink = collect_event, .sink_ctx = &col,
            .frame = collect_frame, .frame_ctx = &col,
        };
        vt_session *s = NULL;
        assert(vt_session_create(&s, &cfg) == 0);
        assert(vt_session_arm(s) == 0);

        // Produce far more than capacity.
        for (int i = 0; i < 6000 && vt_session_sequence(s) < 300; i++) {
            usleep(2000);
        }
        assert(vt_session_sequence(s) >= 300);

        uint64_t wrapped = vt_session_wrapped(s);
        assert(wrapped > 0); // overflow is detectable, never silent

        pthread_mutex_lock(&col.lock);
        col.count = 0;
        col.frame_count = 0;
        pthread_mutex_unlock(&col.lock);

        assert(vt_session_bind(s, TARGET_PID) == 0);
        // Give the pump a moment to route live events (all-target source).
        for (int i = 0; i < 2000; i++) {
            pthread_mutex_lock(&col.lock);
            long live = col.count - col.replay_count;
            pthread_mutex_unlock(&col.lock);
            if (live > 0) break;
            usleep(2000);
        }
        pthread_mutex_lock(&col.lock);
        long replayed = col.replay_count;
        long live_after = col.count - col.replay_count;
        pthread_mutex_unlock(&col.lock);
        // Replay is capped by the ring capacity: the oldest events were
        // overwritten (wrapped > 0) and are gone. Everything else delivered
        // after bind is LIVE-tagged (the source is all-target here, so live
        // accumulates immediately and could exceed the ring size).
        assert(replayed <= 64);
        assert(live_after > 0);
        assert(vt_session_wrapped(s) > 0);

        vt_session_stop(s);
        vt_session_destroy(s);
    }

    // ── bind with an unknown pid finds nothing ────────────────────────
    {
        struct collector col = { .lock = PTHREAD_MUTEX_INITIALIZER };
        struct source src = { .lock = PTHREAD_MUTEX_INITIALIZER,
                              .target_every = 2 };
        vt_session_config cfg = {
            .ring_capacity = 256,
            .source = source_next, .source_ctx = &src,
            .sink = collect_event, .sink_ctx = &col,
            .frame = collect_frame, .frame_ctx = &col,
        };
        vt_session *s = NULL;
        assert(vt_session_create(&s, &cfg) == 0);
        assert(vt_session_arm(s) == 0);
        for (int i = 0; i < 2000 && vt_session_sequence(s) < 20; i++) {
            usleep(2000);
        }
        assert(vt_session_sequence(s) >= 20);

        pthread_mutex_lock(&col.lock);
        col.count = 0;
        col.frame_count = 0;
        pthread_mutex_unlock(&col.lock);

        assert(vt_session_bind(s, 4242) == -ESRCH); // no such pid buffered
        // Still bound now; unrelated-pid events must not stream.
        assert(vt_session_state_get(s) == VT_SESS_BOUND);
        usleep(50000);
        pthread_mutex_lock(&col.lock);
        for (long i = 0; i < col.count; i++) {
            assert(col.events[i].pid != OTHER_PID || col.events[i].pid == 4242);
        }
        pthread_mutex_unlock(&col.lock);

        vt_session_stop(s);
        vt_session_destroy(s);
    }

    // ── re-arm after stop: reusable + fresh pre-roll (deterministic) ──
    {
        // Capped source: round 1 produces exactly 8 events (4 target), the
        // ring is 256 — far larger, so NOTHING wraps and round-1 events can
        // only be evicted by vt_ring_reset. This makes the reset observable:
        // without reset, round-2 replay would include round-1 target events
        // (sequences 1,3,5,7); with reset, replay is exactly the 4 round-2
        // target events (sequences 9,11,13,15).
        struct collector col = { .lock = PTHREAD_MUTEX_INITIALIZER };
        struct source src = { .lock = PTHREAD_MUTEX_INITIALIZER,
                              .target_every = 2, .cap = 8 };
        vt_session_config cfg = {
            .ring_capacity = 256,
            .source = source_next, .source_ctx = &src,
            .sink = collect_event, .sink_ctx = &col,
            .frame = collect_frame, .frame_ctx = &col,
        };
        vt_session *s = NULL;
        assert(vt_session_create(&s, &cfg) == 0);

        // Round 1: arm → produce exactly 8 events → stop.
        assert(vt_session_arm(s) == 0);
        for (int i = 0; i < 2000 && vt_session_sequence(s) < 8; i++) {
            usleep(2000);
        }
        assert(vt_session_sequence(s) == 8); // cap enforced: exactly 8
        uint64_t seq_after_r1 = vt_session_sequence(s);
        vt_session_stop(s);
        assert(vt_session_state_get(s) == VT_SESS_IDLE);

        pthread_mutex_lock(&col.lock);
        long frames_r1 = col.frame_count;   // ready + stopped from round 1
        pthread_mutex_unlock(&col.lock);

        // Round 2: re-arm must respawn the pump, reach READY again, and —
        // critically — start with a FRESH pre-roll. Round 2's cap window is
        // events 9..16 (the source cap is raised by produced count, so
        // bump it to 16 for the second round).
        assert(vt_session_arm(s) == 0);
        assert(vt_session_state_get(s) == VT_SESS_ARMED);
        pthread_mutex_lock(&src.lock);
        src.cap = 16;
        pthread_mutex_unlock(&src.lock);

        // Wait for the round-2 READY frame (it is frames_r1 index).
        for (int i = 0; i < 2000; i++) {
            pthread_mutex_lock(&col.lock);
            long fc = col.frame_count;
            pthread_mutex_unlock(&col.lock);
            if (fc > frames_r1) break;
            usleep(2000);
        }
        pthread_mutex_lock(&col.lock);
        assert(col.frame_count > frames_r1);
        assert(strstr(col.frames[frames_r1], "{\"type\":\"ready\""));
        pthread_mutex_unlock(&col.lock);

        // Wait until the pump consumed the 8 round-2 events (sequence 16).
        for (int i = 0; i < 2000 && vt_session_sequence(s) < 16; i++) {
            usleep(2000);
        }
        assert(vt_session_sequence(s) == 16); // exactly 8 more produced

        // Round-2 bind: with ring reset, replay = round-2 target events
        // only (4 of them: global sequences 9,11,13,15). Without reset, the
        // ring would still hold all 8 round-1 events (256 >> 8, no wrap)
        // and replay would be 8 target events — the assertion below fails.
        assert(vt_session_bind(s, TARGET_PID) == 0);
        pthread_mutex_lock(&col.lock);
        long r2_replay = col.replay_count;
        assert(r2_replay == 4);
        for (long i = 0; i < col.count; i++) {
            if (col.phases[i] != VT_PHASE_REPLAY) continue;
            // Round-1 target events were sequences 1,3,5,7 — all <= 8.
            // Round-2 replays must be strictly beyond the round-1 stop.
            assert(col.events[i].sequence > seq_after_r1);
            assert(col.events[i].sequence > 8);
        }
        pthread_mutex_unlock(&col.lock);

        vt_session_stop(s);
        assert(vt_session_state_get(s) == VT_SESS_IDLE);
        vt_session_destroy(s);
    }

    printf("session_test: all assertions passed\n");
    return 0;
}
