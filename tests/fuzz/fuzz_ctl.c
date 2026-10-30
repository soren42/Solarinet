/* Fuzz the production ctl request path up to (not including) dispatch:
 * ctlReadLine over a real stream socket, then ctlTrimLine, ctlSplitVerb, the
 * k=v argument extractors, the RBAC gate, and the peer authorization matrix.
 * Like tests/unit/test_server_ctl.c this includes solariCtl.c directly so the
 * static functions under test are the shipped ones, not copies. No DB, CA, or
 * listening socket is touched. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define CTL_READ_TIMEOUT_MS 50

#include "solariCtl.c"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum { CTL_FUZZ_LINE_CAP = 16384 };

static void exercise_line(char *line)
{
    static const char *const keys[] = {
        "op", "id", "host", "name", "scope", "token", "pool", "rule", "tier"
    };
    char verb[128];
    char sval[256];
    const char *args = NULL;
    uint64_t u64 = 0;
    uint32_t u32 = 0;
    size_t i;
    ctlVerbClass vc;

    (void)ctlTrimLine(line);
    if (ctlSplitVerb(line, verb, sizeof verb, &args) != SOLARI_OK) return;
    for (i = 0; i < sizeof keys / sizeof keys[0]; ++i) {
        (void)ctlArgStr(args, keys[i], sval, sizeof sval);
        (void)ctlArgU64(args, keys[i], &u64);
        (void)ctlArgU32Hex(args, keys[i], &u32);
    }
    (void)ctlCheckRbac(verb, args, sval, sizeof sval);
    vc = ctlVerbPrivClass(verb);
    /* Invariant: the dashboard never reaches a privileged verb, and an
     * unknown peer reaches nothing, whatever bytes arrived. */
    if (vc == CTL_VC_PRIVILEGED && ctlPeerMayInvoke(CTL_PEER_DASHBOARD, vc))
        __builtin_trap();
    if (ctlPeerMayInvoke(CTL_PEER_UNKNOWN, vc))
        __builtin_trap();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char line[CTL_FUZZ_LINE_CAP];
    int sv[2];
    ssize_t got;

    if (size > sizeof line) size = sizeof line;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 0;
    if (size && write(sv[1], data, size) != (ssize_t)size) {
        close(sv[0]); close(sv[1]);
        return 0;
    }
    shutdown(sv[1], SHUT_WR);   /* EOF instead of a 50 ms stall per input */
    got = ctlReadLine(sv[0], line, sizeof line);
    close(sv[0]); close(sv[1]);
    if (got <= 0) return 0;
    /* Reader contract: exactly one NUL-free, newline-terminated line. */
    if ((size_t)got != strlen(line) || line[got - 1] != '\n')
        __builtin_trap();
    exercise_line(line);
    return 0;
}
