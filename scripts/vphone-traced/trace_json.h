/*
 * trace_json.h — minimal JSON emitters for the stage-2 wire/output format.
 *
 * Records use the blueprint v1.0.0 §18 shape. Only fixed numeric/enum fields
 * are emitted at this stage (no strings, so no escaping is required).
 */

#pragma once

#include <stdint.h>
#include "trace_event.h"

/// `{"protocol":1,"event":"syscall","timestamp":…,"pid":0,"tid":…,
///   "kind":"bsd","phase":"entry","number":5}`  (args omitted in stage 2)
void vt_json_event(char *buf, size_t buflen, const vt_event *ev);

/// `{"protocol":1,"event":"trace_drop","count":N,"timestamp":…}`
void vt_json_drop(char *buf, size_t buflen, uint64_t count, uint64_t ts);

/// `{"protocol":1,"type":"hello","capabilities":["bsd","mach","raw"]}`
void vt_json_hello(char *buf, size_t buflen, int mach);
