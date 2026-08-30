/*
 * host_test.c — native sanity harness for the stage-2 parsing layers.
 *
 * Builds synthetic kd_buf records shaped like kernel output and verifies:
 *   - BSD entry/exit debugid classification and exit-record pid extraction,
 *   - Mach classification with negative SVC numbering semantics,
 *   - TRACE_LOST_EVENTS detection,
 *   - ring wraparound accounting and oldest→newest visit order,
 *   - JSON emitters produce the documented shapes.
 *
 * Not linked against trace_ktrace.c (no kernel calls on the host).
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "trace_event.h"
#include "trace_ring.h"
#include "trace_json.h"

static uint64_t next_ts = 1000;

static vt_kd_buf make_bsd(uint16_t code, uint32_t func, uint64_t a1,
                          uint64_t a2, uint64_t a3, uint64_t a4) {
    return (vt_kd_buf){
        .timestamp = next_ts++,
        .arg1 = a1, .arg2 = a2, .arg3 = a3, .arg4 = a4,
        .arg5 = 0x1122334455,          // thread id
        .debugid = VT_BSDDBG_CODE(VT_DBG_BSD_EXCP_SC, code) | func,
        .cpuid = 1,
    };
}

typedef struct {
    uint64_t seen;
    uint64_t first_arg;
} visit_ctx;

static void visit_cb(const vt_event *e, void *p) {
    visit_ctx *c = p;
    if (c->seen == 0) c->first_arg = e->arg1;
    c->seen++;
}

int main(void) {
    // ── classification ────────────────────────────────────────────────
    // BSD open (5) entry for pid 843: arg4=0 on entry.
    vt_kd_buf entry = make_bsd(5, VT_DBG_FUNC_START, 0x16f00000, 0, 0, 0);
    vt_event ev;
    vt_event_from_record(&entry, 1, &ev);
    assert(ev.kind == VT_KIND_BSD);
    assert(ev.pid == 0);
    assert((ev.debugid & VT_KDBG_FUNC_MASK) == VT_DBG_FUNC_START);
    assert(VT_SYSCALL_NUMBER(ev.debugid) == 5);

    // BSD open exit: arg1=error arg2=rval0 arg3=rval1 arg4=pid.
    vt_kd_buf exit = make_bsd(5, VT_DBG_FUNC_END, 0, 4, 0, 843);
    vt_event_from_record(&exit, 2, &ev);
    assert(ev.kind == VT_KIND_BSD);
    assert(ev.pid == 843);

    // Mach trap (negative SVC, debugid class 1/0x0C).
    vt_kd_buf mach = make_bsd(0, VT_DBG_FUNC_START, 0, 0, 0, 0);
    mach.debugid = VT_MACHDBG_CODE(VT_DBG_MACH_EXCP_SC, 10) | VT_DBG_FUNC_START;
    vt_event_from_record(&mach, 3, &ev);
    assert(ev.kind == VT_KIND_MACH);
    assert(VT_SYSCALL_NUMBER(ev.debugid) == 10);

    // TRACE_DATA_NEWTHREAD layout (xnu thread.c:1597-1602):
    //   arg1=tid arg2=pid arg3=exec-copy arg4=proc_uniqueid.
    // pid-reuse guard: uniqueid must be captured, exec flag is arg3.
    vt_kd_buf nt = (vt_kd_buf){
        .timestamp = next_ts++,
        .arg1 = 0x99, .arg2 = 843, .arg3 = 1, .arg4 = 0x777,
        .arg5 = 0x99,
        .debugid = VT_TRACE_DATA_NEWTHREAD,
        .cpuid = 1,
    };
    vt_event_from_record(&nt, 5, &ev);
    assert(ev.kind == VT_KIND_META);
    assert(ev.tid == 0x99);          // arg1
    assert(ev.pid == 843);           // arg2
    assert(ev.exec_copy == 1);       // arg3
    assert(ev.uniqueid == 0x777);    // arg4

    // Lost events marker.
    vt_kd_buf lost = make_bsd(0, VT_DBG_FUNC_NONE, 1, 0, 0, 0);
    lost.debugid = VT_TRACE_LOST_EVENTS;
    vt_event_from_record(&lost, 4, &ev);
    assert(ev.kind == VT_KIND_META);

    // ── ring wraparound + ordering ────────────────────────────────────
    vt_ring ring;

    assert(vt_ring_init(&ring, 8) == 0);
    for (int i = 0; i < 20; i++) {
        vt_kd_buf r = make_bsd(6, VT_DBG_FUNC_END, (uint64_t)i, 0, 0, 900 + (uint64_t)i);
        vt_event e;
        vt_event_from_record(&r, 0, &e);
        vt_ring_push(&ring, &e);
    }
    assert(ring.sequence == 20);
    assert(ring.wrapped == 12);
    // Oldest retained is i=12; sequences strictly ascending 13..20.
    visit_ctx ctxdata = {0};
    vt_ring_visit(&ring, visit_cb, &ctxdata);
    assert(ctxdata.seen == 8);
    assert(ctxdata.first_arg == 12);

    // ── JSON shapes ───────────────────────────────────────────────────
    char buf[512];
    // Re-derive the BSD exit event (later sections overwrote `ev`).
    vt_event_from_record(&exit, 2, &ev);
    vt_json_event(buf, sizeof(buf), &ev);
    assert(strstr(buf, "\"protocol\":1"));
    assert(strstr(buf, "\"kind\":\"bsd\""));
    assert(strstr(buf, "\"phase\":\"exit\""));
    assert(strstr(buf, "\"number\":5"));
    assert(strstr(buf, "\"pid\":843"));

    vt_json_drop(buf, sizeof(buf), 238, 42);
    assert(strstr(buf, "\"event\":\"trace_drop\""));
    assert(strstr(buf, "\"count\":238"));

    vt_ring_destroy(&ring);
    printf("host_test: all assertions passed\n");
    return 0;
}
