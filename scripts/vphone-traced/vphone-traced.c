/*
 * vphone-traced — raw XNU syscall trace reader for vphone-cli.
 *
 * Stage 2 scope (blueprint v1.0.0):
 *   - NO PID filtering, NO name decoding, NO argument decoding.
 *   - Arms a ktrace/kdebug session (BSD syscall events + thread metadata),
 *     pumps raw kd_buf records, prints one line per event:
 *
 *       ts=<raw timebase> tid=0x%llx debugid=0x%08x
 *
 *   - `--kv` adds kind/phase hints derived purely from the debugid (no
 *     syscall tables involved).
 *
 * Run as root inside the iOS guest. ktrace configuration requires superuser.
 *
 * Build:  make -C scripts/vphone-traced   (cross-compiles with iphoneos SDK)
 * Usage:  vphone-traced [--count N] [--timeout-ms MS] [--buffer-events N]
 *                       [--mach] [--kv] [--quiet] [--json]
 */

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "trace_event.h"
#include "trace_ktrace.h"
#include "trace_json.h"

#define VT_DRAIN_RECORDS 8192            // records per KDREADTR iteration
#define VT_DEFAULT_WAIT_MS 50            // poll cadence

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static const char *kind_name(vt_event_kind kind) {
    switch (kind) {
    case VT_KIND_BSD:  return "BSD";
    case VT_KIND_MACH: return "MACH";
    case VT_KIND_META: return "META";
    default:           return "OTHER";
    }
}

static const char *phase_name(uint32_t debugid) {
    switch (debugid & VT_KDBG_FUNC_MASK) {
    case VT_DBG_FUNC_START: return "entry";
    case VT_DBG_FUNC_END:   return "exit";
    default:                return "";
    }
}

static void print_usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s [options]\n"
        "  --count N            exit after N events (0 = unlimited)\n"
        "  --timeout-ms MS      poll wait between drains (default %d)\n"
        "  --buffer-events N    kernel trace buffer size in events\n"
        "  --mach               also capture Mach trap events\n"
        "  --kv                 annotate kind/phase from debugid\n"
        "  --json               emit one JSON object per event (v1 protocol)\n"
        "  --quiet              suppress warnings\n"
        "  --version            print build info and exit\n",
        argv0, VT_DEFAULT_WAIT_MS);
}

int main(int argc, char **argv) {
    uint64_t count_limit = 0;
    unsigned wait_ms = VT_DEFAULT_WAIT_MS;
    uint32_t buffer_events = 0;   // 0 → vt_ktrace default
    int mach = 0, kv = 0, json = 0, quiet = 0;

    static struct option longopts[] = {
        { "count",         required_argument, NULL, 'c' },
        { "timeout-ms",    required_argument, NULL, 't' },
        { "buffer-events", required_argument, NULL, 'b' },
        { "mach",          no_argument,       NULL, 'm' },
        { "kv",            no_argument,       NULL, 'k' },
        { "json",          no_argument,       NULL, 'j' },
        { "quiet",         no_argument,       NULL, 'q' },
        { "help",          no_argument,       NULL, 'h' },
        { "version",       no_argument,       NULL, 'V' },
        { NULL, 0, NULL, 0 },
    };

    int ch;
    while ((ch = getopt_long(argc, argv, "c:t:b:mkjqhV", longopts, NULL)) != -1) {
        switch (ch) {
        case 'c': count_limit = strtoull(optarg, NULL, 0); break;
        case 't': wait_ms = (unsigned)strtoul(optarg, NULL, 0); break;
        case 'b': buffer_events = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'm': mach = 1; break;
        case 'k': kv = 1; break;
        case 'j': json = 1; break;
        case 'q': quiet = 1; break;
        case 'V':
            printf("vphone-traced stage-2 raw reader\n");
            return 0;
        default:
            print_usage(argv[0]);
            return ch == 'h' ? 0 : 2;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (geteuid() != 0) {
        fprintf(stderr, "[trace error] vphone-traced must run as root "
                        "(ktrace configuration requires superuser)\n");
        return 1;
    }

    vt_ktrace *kt = vt_ktrace_create(buffer_events);
    if (!kt) {
        fprintf(stderr, "[trace error] out of memory\n");
        return 1;
    }

    vt_ktrace_kinds kinds = { .bsd = 1, .mach = mach, .meta = 1 };
    int rc = vt_ktrace_arm(kt, &kinds);
    if (rc != 0) {
        fprintf(stderr, "[trace error] kernel trace session could not be "
                        "armed: %s\n", strerror(-rc));
        vt_ktrace_destroy(kt);
        return 1;
    }
    fprintf(stderr, "[trace] armed (bsd=1 mach=%d meta=1) — printing raw events\n",
            mach);

    vt_kd_buf *records = malloc(VT_DRAIN_RECORDS * sizeof(vt_kd_buf));
    if (!records) {
        fprintf(stderr, "[trace error] out of memory\n");
        vt_ktrace_destroy(kt);
        return 1;
    }

    uint64_t seq = 0;
    uint64_t dropped = 0;
    uint64_t emitted = 0;
    int announced_wait = 0;

    while (!g_stop) {
        if (count_limit && emitted >= count_limit) break;

        (void)vt_ktrace_wait(kt, wait_ms);

        long n = vt_ktrace_read(kt, records, VT_DRAIN_RECORDS);
        if (n < 0) {
            if (!quiet)
                fprintf(stderr, "[trace warning] KDREADTR failed: %s\n",
                        strerror((int)-n));
            continue;
        }
        if (n == 0) {
            if (!quiet && !announced_wait) {
                fprintf(stderr, "[trace] waiting for first events…\n");
                announced_wait = 1;
            }
            continue;
        }
        announced_wait = 0;

        for (long i = 0; i < n; i++) {
            const vt_kd_buf *rec = &records[i];
            seq++;

            if (rec->debugid == VT_TRACE_LOST_EVENTS) {
                dropped++;
                fprintf(stderr,
                        "[trace warning] kernel reported dropped events "
                        "(total %llu)\n", (unsigned long long)dropped);
                if (json) {
                    char line[160];
                    vt_json_drop(line, sizeof(line), dropped,
                                 rec->timestamp);
                    puts(line);
                } else {
                    printf("ts=%llu tid=0x%llx debugid=0x%08x DROP\n",
                           (unsigned long long)rec->timestamp,
                           (unsigned long long)rec->arg5, rec->debugid);
                }
                emitted++;
                continue;
            }

            vt_event ev;
            vt_event_from_record(rec, seq, &ev);

            if (json) {
                char line[512];
                vt_json_event(line, sizeof(line), &ev);
                puts(line);
            } else if (kv) {
                printf("ts=%llu tid=0x%llx debugid=0x%08x a1=0x%llx a2=0x%llx "
                       "a3=0x%llx a4=0x%llx %s %s\n",
                       (unsigned long long)ev.timestamp,
                       (unsigned long long)ev.tid, ev.debugid,
                       (unsigned long long)ev.arg1,
                       (unsigned long long)ev.arg2,
                       (unsigned long long)ev.arg3,
                       (unsigned long long)ev.arg4,
                       kind_name(ev.kind), phase_name(ev.debugid));
            } else {
                // Blueprint Stage-2 output contract:
                //   ts=12340001 tid=0x81 debugid=0x040c0014
                printf("ts=%llu tid=0x%llx debugid=0x%08x\n",
                       (unsigned long long)ev.timestamp,
                       (unsigned long long)ev.tid, ev.debugid);
            }
            emitted++;

            if (count_limit && emitted >= count_limit) break;
        }
        fflush(stdout);
    }

    fprintf(stderr, "[trace] stopping after %llu events "
                    "(%llu kernel drop markers)\n",
            (unsigned long long)emitted, (unsigned long long)dropped);

    free(records);
    vt_ktrace_destroy(kt);
    return 0;
}
