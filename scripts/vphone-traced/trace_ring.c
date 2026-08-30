/*
 * trace_ring.c — bounded pre-roll ring buffer for raw trace events.
 */

#include "trace_ring.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static uint64_t round_up_pow2(uint64_t v) {
    if (v == 0) return 1;
    uint64_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

int vt_ring_init(vt_ring *ring, uint64_t capacity) {
    memset(ring, 0, sizeof(*ring));
    ring->capacity = round_up_pow2(capacity);
    ring->mask = ring->capacity - 1;
    ring->events = calloc((size_t)ring->capacity, sizeof(vt_event));
    if (!ring->events) return -ENOMEM;
    if (pthread_mutex_init(&ring->lock, NULL) != 0) {
        free(ring->events);
        ring->events = NULL;
        return -errno;
    }
    return 0;
}

void vt_ring_destroy(vt_ring *ring) {
    if (ring->events) {
        pthread_mutex_destroy(&ring->lock);
        free(ring->events);
        ring->events = NULL;
    }
}

void vt_ring_push(vt_ring *ring, vt_event *event) {
    pthread_mutex_lock(&ring->lock);
    uint64_t seq = ring->sequence + 1;
    event->sequence = seq;
    ring->sequence = seq;

    uint64_t slot = ring->write_index & ring->mask;
    ring->events[slot] = *event;
    ring->write_index++;

    // Once more than capacity pushes have happened, each new push overwrote
    // the oldest retained event.
    if (ring->write_index > ring->capacity) ring->wrapped++;
    pthread_mutex_unlock(&ring->lock);
}

uint64_t vt_ring_count(const vt_ring *ring) {
    pthread_mutex_lock(&((vt_ring *)ring)->lock);
    uint64_t total = ring->write_index;
    pthread_mutex_unlock(&((vt_ring *)ring)->lock);
    return total > ring->capacity ? ring->capacity : total;
}

/// Visit takes the ring lock and copies each retained event out before
/// invoking the callback, so the callback observes stable data and never
/// runs concurrently with a push that could overwrite the slot mid-walk.
/// The callback may therefore take other locks (e.g. the session lock)
/// without risking deadlock against the ring.
void vt_ring_visit(const vt_ring *ring, vt_ring_visit_fn fn, void *ctx) {
    vt_event snapshot;
    uint64_t count;
    uint64_t start;

    pthread_mutex_lock(&((vt_ring *)ring)->lock);
    count = ring->write_index > ring->capacity ? ring->capacity : ring->write_index;
    start = ring->write_index - count;
    for (uint64_t i = 0; i < count; i++) {
        snapshot = ring->events[(start + i) & ring->mask];
        pthread_mutex_unlock(&((vt_ring *)ring)->lock);
        fn(&snapshot, ctx);
        pthread_mutex_lock(&((vt_ring *)ring)->lock);
    }
    pthread_mutex_unlock(&((vt_ring *)ring)->lock);
}
