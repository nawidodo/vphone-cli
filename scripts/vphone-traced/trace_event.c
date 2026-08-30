/*
 * trace_event.c — record classification for vphone-traced.
 */

#include "trace_event.h"

static vt_event_kind vt_classify(uint32_t debugid) {
    if (VT_IS_BSD_SYSCALL(debugid)) return VT_KIND_BSD;
    if (VT_IS_MACH_SYSCALL(debugid)) return VT_KIND_MACH;
    if ((debugid & 0xff000000u) == (VT_DBG_TRACE << 24)) return VT_KIND_META;
    return VT_KIND_OTHER;
}

void vt_event_newthread_pid(const vt_kd_buf *rec, uint32_t *pid_out,
                            int *exec_copy_out, uint64_t *uniqueid_out) {
    // TRACE_DATA_NEWTHREAD (xnu osfmk/kern/thread.c:1597-1602):
    //   KDBG_RELEASE(TRACE_DATA_NEWTHREAD, thread_tid, args[1], args[2], args[3])
    // where kdbg_trace_data() fills args[1]=pid, args[3]=proc_uniqueid, and
    // args[2] = task_is_exec_copy(parent_task) ? 1 : 0. Through KDBG_RELEASE
    // those land in the record as:
    //   record.arg1 = tid
    //   record.arg2 = pid
    //   record.arg3 = exec-copy flag
    //   record.arg4 = proc uniqueid (stable across pid reuse)
    if (pid_out) *pid_out = (uint32_t)rec->arg2;
    if (exec_copy_out) *exec_copy_out = (rec->arg3 != 0);
    if (uniqueid_out) *uniqueid_out = rec->arg4;
}

void vt_event_from_record(const vt_kd_buf *rec, uint64_t sequence,
                          vt_event *out) {
    out->sequence = sequence;
    out->timestamp = rec->timestamp;
    out->tid = rec->arg5;
    out->pid = 0;
    out->uniqueid = 0;
    out->exec_copy = 0;
    out->debugid = rec->debugid;
    out->cpu = (uint8_t)rec->cpuid;
    out->kind = vt_classify(rec->debugid);
    out->arg1 = rec->arg1;
    out->arg2 = rec->arg2;
    out->arg3 = rec->arg3;
    out->arg4 = rec->arg4;

    // BSD syscall exit records carry the pid in arg4 (systemcalls.c):
    //   arg1=error arg2=rval0 arg3=rval1 arg4=pid
    if (out->kind == VT_KIND_BSD &&
        (rec->debugid & VT_KDBG_FUNC_MASK) == VT_DBG_FUNC_END) {
        out->pid = (uint32_t)rec->arg4;
    }

    // TRACE_DATA_NEWTHREAD: arg1=tid arg2=pid arg3=exec-copy arg4=uniqueid.
    if (rec->debugid == VT_TRACE_DATA_NEWTHREAD) {
        uint32_t pid = 0;
        int exec_copy = 0;
        uint64_t uniqueid = 0;
        vt_event_newthread_pid(rec, &pid, &exec_copy, &uniqueid);
        out->pid = pid;
        out->exec_copy = exec_copy ? 1 : 0;
        out->uniqueid = uniqueid;
    }
}
