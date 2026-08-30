/*
 * trace_event.h — normalized raw trace event + kernel ABI constants.
 *
 * The guest iOS SDK does not ship <sys/kdebug_private.h>, so the kd_buf ABI
 * and the kdebug event-class constants used by the reader are vendored here.
 * Constants verified against xnu-11417.140.69 (host) and expected identical
 * on xnu-12377.x (iOS 26.x guest); see docs/syscall-tracer-architecture-review.md.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

// ── kdebug event-id layout (bsd/sys/kdebug.h) ────────────────────────────

#define VT_KDBG_CLASS_OFFSET    (24)
#define VT_KDBG_SUBCLASS_OFFSET (16)
#define VT_KDBG_CODE_OFFSET     (2)
#define VT_KDBG_FUNC_MASK       (0x00000003)

#define VT_KDBG_EVENTID(Class, SubClass, Code)                       \
    (((uint32_t)((Class)    & 0xff) << VT_KDBG_CLASS_OFFSET)    |     \
     ((uint32_t)((SubClass) & 0xff) << VT_KDBG_SUBCLASS_OFFSET) |     \
     ((uint32_t)((Code)     & 0x3fff) << VT_KDBG_CODE_OFFSET))

#define VT_DBG_FUNC_START (1U)
#define VT_DBG_FUNC_END   (2U)
#define VT_DBG_FUNC_NONE  (0U)

// Classes / subclasses of interest.
#define VT_DBG_BSD       (4)
#define VT_DBG_MACH      (1)
#define VT_DBG_TRACE     (7)
#define VT_DBG_BSD_EXCP_SC  (0x0C)
#define VT_DBG_MACH_EXCP_SC (0x0C)
#define VT_DBG_TRACE_DATA   (0)
#define VT_DBG_TRACE_STRING (1)
#define VT_DBG_TRACE_INFO   (2)

#define VT_BSDDBG_CODE(SubClass, Code) \
    VT_KDBG_EVENTID(VT_DBG_BSD, SubClass, Code)
#define VT_MACHDBG_CODE(SubClass, Code) \
    VT_KDBG_EVENTID(VT_DBG_MACH, SubClass, Code)
#define VT_TRACEDBG_CODE(SubClass, Code) \
    VT_KDBG_EVENTID(VT_DBG_TRACE, SubClass, Code)

// Metadata debugids the reader keys on.
#define VT_TRACE_DATA_NEWTHREAD   VT_TRACEDBG_CODE(VT_DBG_TRACE_DATA, 1)   // 0x07000004
#define VT_TRACE_DATA_EXEC        VT_TRACEDBG_CODE(VT_DBG_TRACE_DATA, 2)   // 0x07000008
#define VT_TRACE_STRING_NEWTHREAD VT_TRACEDBG_CODE(VT_DBG_TRACE_STRING, 1) // 0x07010004
#define VT_TRACE_LOST_EVENTS      VT_TRACEDBG_CODE(VT_DBG_TRACE_INFO, 2)   // 0x07020008

// BSD/Mach syscall debugid tests.
#define VT_IS_BSD_SYSCALL(debugid)                                              \
    (((debugid) & 0xff000000u) == (VT_DBG_BSD << 24) &&                          \
     ((debugid) & 0x00ff0000u) == (VT_DBG_BSD_EXCP_SC << 16))
#define VT_IS_MACH_SYSCALL(debugid)                                             \
    (((debugid) & 0xff000000u) == (VT_DBG_MACH << 24) &&                         \
     ((debugid) & 0x00ff0000u) == (VT_DBG_MACH_EXCP_SC << 16))
#define VT_SYSCALL_NUMBER(debugid) \
    ((int32_t)(((debugid) >> VT_KDBG_CODE_OFFSET) & 0x3fff))

// ── kernel raw record ABI (bsd/sys/kdebug_private.h, arm64) ─────────────

// Exactly the kernel's `kd_buf` layout on arm64: 64 bytes
// (8 + 5×8 args + 4 debugid + 4 cpuid + 8 unused; no padding).
// Kernel arg fields are uint64_t on arm64 (kdebug_private.h:253).
typedef uint64_t vt_kd_arg_t;
typedef struct {
    uint64_t     timestamp;
    vt_kd_arg_t  arg1;
    vt_kd_arg_t  arg2;
    vt_kd_arg_t  arg3;
    vt_kd_arg_t  arg4;
    vt_kd_arg_t  arg5;   // always the thread ID
    uint32_t     debugid;
    uint32_t     cpuid;
    uint64_t     unused;
} vt_kd_buf;

_Static_assert(sizeof(vt_kd_buf) == 64, "kd_buf ABI drift");

// ── normalized event consumed by the ring/printer ────────────────────────

typedef enum {
    VT_KIND_BSD = 0,
    VT_KIND_MACH = 1,
    VT_KIND_META = 2,   // newthread/exec/lost-events bookkeeping
    VT_KIND_OTHER = 3,
} vt_event_kind;

typedef struct {
    uint64_t      sequence;   // daemon-local, monotonically increasing
    uint64_t      timestamp;  // kernel timebase (absolute, unconverted)
    uint64_t      tid;        // arg5 of the record
    uint32_t      pid;        // 0 when not yet resolvable
    uint64_t      uniqueid;   // proc_uniqueid from NEWTHREAD records (pid-reuse guard)
    uint32_t      debugid;
    uint8_t       cpu;
    uint8_t       exec_copy;  // 1 when the NEWTHREAD record is an exec copy
    vt_event_kind kind;
    vt_kd_arg_t   arg1;
    vt_kd_arg_t   arg2;
    vt_kd_arg_t   arg3;
    vt_kd_arg_t   arg4;
} vt_event;
/// Extract (pid, exec-copy flag, proc uniqueid) carried by
/// TRACE_DATA_NEWTHREAD records. Layout per xnu thread.c:1597-1602:
/// arg1=tid, arg2=pid, arg3=exec-copy, arg4=proc_uniqueid.
void vt_event_newthread_pid(const vt_kd_buf *rec, uint32_t *pid_out,
                            int *exec_copy_out, uint64_t *uniqueid_out);

/// Classify a raw kernel record into a normalized event.
void vt_event_from_record(const vt_kd_buf *rec, uint64_t sequence,
                          vt_event *out);
