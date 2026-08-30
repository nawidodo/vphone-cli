/*
 * trace_json.c — minimal JSON emitters (numeric fields only; no escaping).
 */

#include "trace_json.h"
#include <stdio.h>

static const char *kind_str(vt_event_kind kind) {
    switch (kind) {
    case VT_KIND_BSD:  return "bsd";
    case VT_KIND_MACH: return "mach";
    case VT_KIND_META: return "meta";
    default:           return "other";
    }
}

static const char *phase_str(uint32_t debugid) {
    switch (debugid & VT_KDBG_FUNC_MASK) {
    case VT_DBG_FUNC_START: return "entry";
    case VT_DBG_FUNC_END:   return "exit";
    default:                return "none";
    }
}

void vt_json_event(char *buf, size_t buflen, const vt_event *ev) {
    int number = VT_IS_BSD_SYSCALL(ev->debugid) || VT_IS_MACH_SYSCALL(ev->debugid)
        ? VT_SYSCALL_NUMBER(ev->debugid)
        : -1;

    snprintf(buf, buflen,
        "{\"protocol\":1,\"event\":\"syscall\",\"sequence\":%llu,"
        "\"timestamp\":%llu,\"pid\":%u,\"tid\":%llu,"
        "\"kind\":\"%s\",\"phase\":\"%s\",\"number\":%d}",
        (unsigned long long)ev->sequence,
        (unsigned long long)ev->timestamp, ev->pid,
        (unsigned long long)ev->tid,
        kind_str(ev->kind), phase_str(ev->debugid), number);
}

void vt_json_drop(char *buf, size_t buflen, uint64_t count, uint64_t ts) {
    snprintf(buf, buflen,
        "{\"protocol\":1,\"event\":\"trace_drop\",\"count\":%llu,"
        "\"timestamp\":%llu}",
        (unsigned long long)count, (unsigned long long)ts);
}

void vt_json_hello(char *buf, size_t buflen, int mach) {
    snprintf(buf, buflen,
        "{\"protocol\":1,\"type\":\"hello\","
        "\"capabilities\":[\"bsd\",\"%s\",\"raw\",\"pid-filter\"]}",
        mach ? "mach" : "raw");
}
