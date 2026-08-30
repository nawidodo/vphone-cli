/*
 * trace_ring.h — bounded pre-roll ring buffer for raw trace events.
 *
 * Single-writer / multi-reader safe under `lock`. Capture (ARMED phase)
 * pushes every event; BIND snapshots `write_index` + `sequence` and the
 * replay walks oldest→newest. Wraparound overwrites the oldest entries and
 * bumps `wrapped` so the loss is reportable, never silent.
 */

#pragma once

#include <stdint.h>
#include <pthread.h>
#include "trace_event.h"

typedef struct vt_ring {
    pthread_mutex_t lock;
    vt_event       *events;
    uint64_t        capacity;     // power of two
    uint64_t        mask;         // capacity - 1
    uint64_t        write_index;  // total pushes ever (monotonic)
    uint64_t        sequence;     // last assigned sequence number
    uint64_t        wrapped;      // count of overwritten pre-bind events
} vt_ring;

/// Allocate a ring of `capacity` events (rounded up to a power of two).
/// Returns 0 on success, -errno on failure.
int vt_ring_init(vt_ring *ring, uint64_t capacity);

/// Free ring storage; safe to call twice.
void vt_ring_destroy(vt_ring *ring);

/// Append an event. Sequence ownership: if the caller pre-stamped
/// `event->sequence` (nonzero — the session pump stamps under its own lock),
/// the ring keeps it and mirrors it; only unstamped events (sequence == 0)
/// get the next ring-local sequence. Overwrites the oldest entry once full,
/// incrementing `wrapped`. Single-writer only (the reader thread).
void vt_ring_push(vt_ring *ring, vt_event *event);

/// Number of events currently retained (never exceeds capacity).
uint64_t vt_ring_count(const vt_ring *ring);

/// Drop all retained events and restore the ring to empty. `sequence_base`
/// is the session's current sequence watermark: the ring's own sequence
/// mirror is set to it so pushes continue the global numbering. The caller
/// must hold no ring-internal expectations; the ring takes its own lock.
void vt_ring_reset(vt_ring *ring, uint64_t sequence_base);

/// Invoke `fn` over retained events ordered oldest → newest. The ring takes
/// its own lock: each event is copied out before the callback runs, so the
/// callback observes a stable snapshot and may take other locks freely.
typedef void (*vt_ring_visit_fn)(const vt_event *event, void *ctx);
void vt_ring_visit(const vt_ring *ring, vt_ring_visit_fn fn, void *ctx);
