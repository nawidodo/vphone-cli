/*
 * trace_ktrace.c — XNU ktrace/kdebug session control.
 *
 * Session lifecycle follows the ktrace(1)/libktrace pattern:
 *
 *   KERN_KDSETBUF      size the trace buffer (events)
 *   KERN_KDSETUP       allocate (kdbg_reinit; requires buffers enabled flag)
 *   KERN_KDSETREG      debugid range/subclass filter (kd_regtype)
 *   KERN_KDENABLE      enable with KDEBUG_ENABLE_TRACE (0x1)
 *   KERN_KDBUFWAIT     block until storage threshold or timeout
 *   KERN_KDREADTR      drain merged records (48-byte kd_buf each)
 *   KERN_KDREADCURTHRMAP  live tid->pid/comm snapshot
 *   KERN_KDREMOVE      tear down
 *
 * Configure/read require superuser (ktrace_configure → EPERM otherwise).
 */

#include "trace_ktrace.h"
#include "trace_event.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

// Guest SDK <sys/sysctl.h> provides KERN_KD* opcodes. KDBG flag constants and
// kd_regtype live in the private header, so they are vendored here.

#define VT_KDBG_BUFINIT      0x80000000U
#define VT_KDBG_NOWRAP       0x0002U
#define VT_KDBG_CONTINUOUS_T 0x0004U

// kd_regtype: bsd/sys/kdebug_private.h:467-473
typedef struct {
    unsigned int type;
    unsigned int value1;
    unsigned int value2;
    unsigned int value3;
    unsigned int value4;
} vt_kd_regtype;

#define VT_KDBG_CLASSTYPE  0x10000
#define VT_KDBG_SUBCLSTYPE 0x20000
#define VT_KDBG_RANGETYPE  0x40000
#define VT_KDBG_TYPENONE   0x80000

#define VT_KDEBUG_ENABLE_TRACE 0x1U

// kbufinfo_t (BSD/sys/kdebug_private.h) layout needed by KERN_KDGETBUF.
typedef struct {
    uint32_t  nkdbufs;
    uint32_t  nkdthreads;
    uint32_t  bufunitsize;
    uint32_t  version;
    uint32_t  cpudatasize;
    uint32_t  flags;
    uint32_t  threadmapsize;
    uint32_t  todsecs;
    uint32_t  todusecs;
    uint32_t  future_use[16];
} vt_kbufinfo_t;

struct vt_ktrace {
    uint32_t buffer_events;
    uint64_t raw_buf_bytes;
    uint8_t *raw_buf;
};

vt_ktrace *vt_ktrace_create(uint32_t buffer_events) {
    vt_ktrace *kt = calloc(1, sizeof(*kt));
    if (!kt) return NULL;
    kt->buffer_events = buffer_events ? buffer_events : (1u << 20); // 1M events default
    kt->raw_buf_bytes = 64u * 1024u * 1024u;                       // 64 MiB drain buffer
    kt->raw_buf = malloc(kt->raw_buf_bytes);
    if (!kt->raw_buf) {
        free(kt);
        return NULL;
    }
    return kt;
}

static int kdctl(vt_ktrace *kt, unsigned op, const void *in, size_t in_len,
                 void *out, size_t *out_len) {
    int mib[3] = { CTL_KERN, KERN_KDEBUG, (int)op };
    size_t len = out_len ? *out_len : 0;
    // The sysctl handler reads `req->newptr` for set-ops (in) and writes
    // `req->oldptr` for get-ops (out); both share the same name list.
    void *newp = (void *)(uintptr_t)in;
    size_t newlen = in_len;
    void *oldp = out;
    size_t *oldlenp = out_len ? &len : NULL;
    if (out_len) *out_len = 0;

    int rc = sysctl(mib, 3, oldp, oldlenp, newp, newlen);
    if (out_len) *out_len = len;
    (void)kt;
    return rc == 0 ? 0 : -errno;
}

int vt_ktrace_arm(vt_ktrace *kt, const vt_ktrace_kinds *kinds) {
    if (!kt || !kinds) return -EINVAL;

    // 1. Size the buffer. KERN_KDSETBUF takes the value via newptr.
    int v = (int)kt->buffer_events;
    int rc = kdctl(kt, KERN_KDSETBUF, &v, sizeof(v), NULL, NULL);
    if (rc == 0) {
        // Mark buffers enabled so KERN_KDSETUP's kdbg_reinit proceeds.
        // KERN_KDEFLAGS ORs KDBG_BUFINIT into kdc_flags.
        unsigned flags = VT_KDBG_BUFINIT;
        rc = kdctl(kt, KERN_KDEFLAGS, &flags, sizeof(flags), NULL, NULL);
    }
    if (rc == 0) {
        rc = kdctl(kt, KERN_KDSETUP, NULL, 0, NULL, NULL);
    }
    if (rc != 0) return rc;

    // 2. Event filter. Prefer subclass registration; fall back to a range.
    //    Capture pattern (per kind, both syscall classes use subclass 0x0C):
    //      KDBG_SUBCLSTYPE {class, subclass} → [EVENTID(class, sub, 0),
    //                                           EVENTID(class, sub+1, 0))
    int have_filter = 0;
    if (kinds->bsd) {
        vt_kd_regtype reg = {
            .type = VT_KDBG_SUBCLSTYPE,
            .value1 = VT_DBG_BSD,
            .value2 = VT_DBG_BSD_EXCP_SC,
        };
        rc = kdctl(kt, KERN_KDSETREG, &reg, sizeof(reg), NULL, NULL);
        if (rc == 0) have_filter = 1;
    }
    if (!have_filter && kinds->mach) {
        vt_kd_regtype reg = {
            .type = VT_KDBG_SUBCLSTYPE,
            .value1 = VT_DBG_MACH,
            .value2 = VT_DBG_MACH_EXCP_SC,
        };
        rc = kdctl(kt, KERN_KDSETREG, &reg, sizeof(reg), NULL, NULL);
        if (rc == 0) have_filter = 1;
    }
    if (!have_filter) {
        // No subclass filter (e.g. bsd==mach==0): capture everything in the
        // DBG_BSD..DBG_MISC class span so meta records still flow.
        vt_kd_regtype reg = {
            .type = VT_KDBG_RANGETYPE,
            .value1 = 0x00000000,
            .value2 = 0xFFFFFFFF,
        };
        rc = kdctl(kt, KERN_KDSETREG, &reg, sizeof(reg), NULL, NULL);
        if (rc != 0) return rc;
    }

    // 3. Enable.
    v = (int)VT_KDEBUG_ENABLE_TRACE;
    return kdctl(kt, KERN_KDENABLE, &v, sizeof(v), NULL, NULL);
}

int vt_ktrace_wait(vt_ktrace *kt, unsigned timeout_ms) {
    if (!kt) return -EINVAL;
    // KERN_KDBUFWAIT: the request timeout (ms) is supplied via oldptr with
    // oldlen holding its size; on return the boolean "threshold exceeded"
    // is stored back (see kdbg_sysctl: *sizep = kdbg_wait(size)). We pass
    // the timeout through the old buffer and read the result from it.
    uint64_t t = timeout_ms;
    size_t len = sizeof(t);
    int mib[3] = { CTL_KERN, KERN_KDEBUG, KERN_KDBUFWAIT };
    int rc = sysctl(mib, 3, &t, &len, NULL, 0);
    if (rc != 0) return -errno;
    return t != 0;
}

long vt_ktrace_read(vt_ktrace *kt, vt_kd_buf *out, size_t max_records) {
    if (!kt || !out || max_records == 0) return -EINVAL;

    size_t want = kt->raw_buf_bytes;
    // KERN_KDREADTR copies merged kd_buf records; on input oldlen is the
    // buffer capacity, on output the number of bytes written. EBUSY from the
    // kernel means "more events than fit" — caller re-issues.
    int mib[3] = { CTL_KERN, KERN_KDEBUG, KERN_KDREADTR };
    size_t got = want;
    int rc = sysctl(mib, 3, kt->raw_buf, &got, NULL, 0);
    if (rc != 0) {
        if (errno == EINVAL && got == 0) return 0;  // nothing buffered yet
        return -errno;
    }
    if (got == 0 || got % sizeof(vt_kd_buf) != 0) return 0;

    size_t n = got / sizeof(vt_kd_buf);
    if (n > max_records) n = max_records;
    memcpy(out, kt->raw_buf, n * sizeof(vt_kd_buf));
    return (long)n;
}

long vt_ktrace_thread_map(vt_ktrace *kt, vt_thread_map_entry *out,
                          size_t max_entries) {
    if (!kt || !out || max_entries == 0) return -EINVAL;
    // kd_threadmap (arm64, kdebug_private.h:476-487):
    //   uint64 thread; int32 pid; char command[20]  → 32-byte stride.
    // pid field: pid >= 1 (1 == kernproc), 0 marks a dead entry.
    size_t bytes = max_entries * 32;
    uint8_t *buf = malloc(bytes);
    if (!buf) return -ENOMEM;

    int mib[3] = { CTL_KERN, KERN_KDEBUG, KERN_KDREADCURTHRMAP };
    size_t got = bytes;
    int rc = sysctl(mib, 3, buf, &got, NULL, 0);
    if (rc != 0) {
        free(buf);
        return -errno;
    }

    long n = 0;
    for (size_t off = 0; off + 32 <= got && (size_t)n < max_entries; off += 32) {
        uint64_t tid;
        int32_t pid;
        memcpy(&tid, buf + off, 8);
        memcpy(&pid, buf + off + 8, 4);
        out[n].tid = tid;
        out[n].pid = pid;
        memcpy(out[n].comm, buf + off + 12, 20);
        n++;
    }
    free(buf);
    return n;
}

void vt_ktrace_destroy(vt_ktrace *kt) {
    if (!kt) return;
    int zero = 0;
    (void)kdctl(kt, KERN_KDENABLE, &zero, sizeof(zero), NULL, NULL);
    (void)kdctl(kt, KERN_KDREMOVE, NULL, 0, NULL, NULL);
    free(kt->raw_buf);
    free(kt);
}
