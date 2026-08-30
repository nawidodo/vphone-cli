# vphone-traced — iOS guest raw syscall trace reader (stage 2).
# Pure C: no Foundation, no frameworks, links only libSystem.

SRCS := vphone-traced.c trace_event.c trace_ring.c trace_ktrace.c trace_json.c trace_session.c
HDRS := trace_event.h trace_ring.h trace_ktrace.h trace_json.h trace_session.h
OUT  := vphone-traced

CC := xcrun -sdk iphoneos clang
CFLAGS := -arch arm64 -Os -Wall -Wextra -Werror \
          -fvisibility=hidden -fno-objc-arc \
          -DVT_BUILD_HASH='"$(GIT_HASH)"'
LDFLAGS := -arch arm64

.PHONY: all clean hosttest

all: $(OUT)

$(OUT): $(SRCS) $(HDRS)
	@echo "=== Building vphone-traced (arm64, iphoneos) ==="
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)
	@echo "  built OK"

# Native build/run of the record-parsing layers (no kernel session): exercises
# classification + JSON on synthetic records under the macOS SDK.
HOSTSRCS := trace_event.c trace_ring.c trace_json.c host_test.c
hosttest: $(HOSTSRCS) $(HDRS)
	clang -g -O0 -Wall -Wextra -Werror -o $@.out $(HOSTSRCS)
	./$@.out

# Native launch-time state-machine harness (synthetic source, no kernel):
# ARM/READY/pre-roll/bind/replay/live/wraparound guards (blueprint v1.1.0 §23).
SESSIONSRCS := trace_event.c trace_ring.c trace_session.c trace_json.c session_test.c
sessiontest: $(SESSIONSRCS) $(HDRS)
	clang -g -O0 -Wall -Wextra -Werror -o $@.out $(SESSIONSRCS) -lpthread
	./$@.out

clean:
	rm -f $(OUT) hosttest.out
