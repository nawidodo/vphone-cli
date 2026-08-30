/*
 * trace_ktrace.h — XNU ktrace/kdebug session control (raw record pump).
 *
 * Drives the CTL_KERN/KERN_KDEBUG sysctl interface. All MIB opcodes exist in
 * the guest SDK (<sys/sysctl.h>); the private kd_buf ABI and event constants
 * live in trace_event.h.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "trace_event.h"

typedef struct vt_ktrace vt_ktrace;

/// Which event families to subscribe to (debugid range / subclass filter).
typedef struct {
    int bsd;    // BSD syscall entry/exit (DBG_BSD/DBG_BSD_EXCP_SC)
    int mach;   // Mach trap entry/exit (DBG_MACH/DBG_MACH_EXCP_SC)
    int meta;   // TRACE_DATA_*/TRACE_STRING_* thread bookkeeping (always on for pid resolution)
} vt_ktrace_kinds;

/// Allocate a session. `buffer_events` is the requested kernel trace buffer
/// size in events; 0 selects the default.
vt_ktrace *vt_ktrace_create(uint32_t buffer_events);

/// Configure and enable tracing for the requested event kinds. Returns 0 on
/// success, -errno (EPERM without root; EINVAL on unsupported kernels) on
/// failure.
int vt_ktrace_arm(vt_ktrace *kt, const vt_ktrace_kinds *kinds);

/// Block until at least one storage unit has data or `timeout_ms` elapses.
/// Returns 1 if data is likely available, 0 on timeout, -errno on error.
int vt_ktrace_wait(vt_ktrace *kt, unsigned timeout_ms);

/// Drain available raw records into `out` (capacity `max_records`).
/// Returns the number of records written (0 when drained), -errno on error.
/// Blocking wait is performed by vt_ktrace_wait, not here.
long vt_ktrace_read(vt_ktrace *kt, vt_kd_buf *out, size_t max_records);

/// Snapshot the live thread map (tid -> pid + 20-byte comm).
/// Returns the number of entries written, -errno on error.
typedef struct {
    uint64_t tid;
    int32_t  pid;
    char     comm[20];
} vt_thread_map_entry;

long vt_ktrace_thread_map(vt_ktrace *kt, vt_thread_map_entry *out,
                          size_t max_entries);

/// Disable and release the kernel trace session.
void vt_ktrace_destroy(vt_ktrace *kt);
