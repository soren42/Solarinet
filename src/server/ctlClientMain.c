/*
 * ctlClientMain.c - solariCtlClient: standard-C client for the solariCtl
 * operator bridge (AF_UNIX line protocol, §9.1/§11.1).
 *
 * The deploy scripts previously embedded a python3 helper to speak the SIGN
 * verb; per the project contract the application is strictly standard C, so
 * this tool replaces that helper. It speaks the same one-line protocol the
 * bridge defines ("VERB k=v ...\n" -> "OK ...\n" | "ERR ...\n"):
 *
 *   solariCtlClient --sock PATH sign --op OPERATOR --csr FILE --out FILE
 *       sends  SIGN op=<op> csr=<pct-encoded CSR PEM>
 *       expects OK cert=<pct-encoded cert PEM>, decodes it into FILE.
 *
 *   solariCtlClient --sock PATH ping
 *       liveness probe; exits 0 on "OK".
 *
 *   solariCtlClient --sock PATH revoke-cert --op OP --node ID --serial HEX
 *       --fingerprint SHA256 --reason REASON [--confirm TOKEN]
 *       first call returns a ten-minute confirmation token; repeat the exact
 *       arguments with --confirm to record revocation and retire that identity.
 *
 * The percent-encoding mirrors solariCtl.c's ctlPctEncode/ctlPctDecode exactly
 * (bare-token set [A-Za-z0-9._:/@-], everything else %XX): values must not
 * contain the protocol's separators (space/tab/newline), and PEM bodies carry
 * '+', '=', and newlines, so full encoding is required on both directions.
 * Exit codes: 0 ok, 1 ERR reply / bad reply, 2 usage, 3 I/O or socket failure.
 */
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define CTL_MAX_LINE (256 * 1024)   /* generous: CSR/cert PEMs are a few KB */
#define CTL_REQUEST_CAP 16384        /* matches solariCtl.c CTL_REQ_CAP */

/* URL-encode `in` into `out` (cap incl. NUL): keep the bare-token set the
 * bridge's parser treats as safe, %XX everything else. Mirrors ctlPctEncode.
 * Returns bytes written (excl. NUL) or 0 on overflow. */
static size_t pctEncode(const char *in, char *out, size_t cap)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (; in && *in; in++) {
        unsigned char c = (unsigned char)*in;
        int bare = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                   c == ':' || c == '/' || c == '@' || c == '-';
        if (bare) {
            if (o + 1 >= cap) return 0;
            out[o++] = (char)c;
        } else {
            if (o + 3 >= cap) return 0;
            out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 0xF];
        }
    }
    if (o >= cap) return 0;
    out[o] = '\0';
    return o;
}

/* In-place percent-decode (%XX -> byte); malformed %XX left untouched.
 * Mirrors ctlPctDecode. */
static void pctDecode(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (r[0] == '%' && isxdigit((unsigned char)r[1]) &&
            isxdigit((unsigned char)r[2])) {
            int hi = isdigit((unsigned char)r[1]) ? r[1] - '0'
                       : (tolower((unsigned char)r[1]) - 'a' + 10);
            int lo = isdigit((unsigned char)r[2]) ? r[2] - '0'
                       : (tolower((unsigned char)r[2]) - 'a' + 10);
            *w++ = (char)((hi << 4) | lo);
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/* Read an entire (small) file into a malloc'd NUL-terminated buffer. */
static char *readFile(const char *path, size_t *lenOut)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long  n;

    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || n > CTL_MAX_LINE) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f);
        return NULL;
    }
    fclose(f);
    buf[n] = '\0';
    if (lenOut) *lenOut = (size_t)n;
    return buf;
}

/* Send one request line over the AF_UNIX socket and read the one-line reply
 * into reply (cap incl. NUL, '\n' stripped). Returns 0 ok, -1 on I/O error. */
static int ctlRoundTrip(const char *sockPath, const char *req,
                        char *reply, size_t cap)
{
    struct sockaddr_un sa;
    int    fd;
    size_t off = 0, len = strlen(req);

    if (strlen(sockPath) >= sizeof sa.sun_path) return -1;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, sockPath, sizeof sa.sun_path - 1);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return -1;
    }
    while (off < len) {
        ssize_t w = write(fd, req + off, len - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { close(fd); return -1; }
        off += (size_t)w;
    }
    off = 0;
    int complete = 0;
    for (;;) {
        ssize_t r;
        char   *nl;
        if (off + 1 >= cap) { close(fd); return -1; }
        r = read(fd, reply + off, cap - 1 - off);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) { close(fd); return -1; }
        if (r == 0) break;
        off += (size_t)r;
        reply[off] = '\0';
        nl = strchr(reply, '\n');
        if (nl) { *nl = '\0'; complete = 1; break; }
    }
    close(fd);
    return complete ? 0 : -1;
}

static void usage(const char *a0)
{
    fprintf(stderr,
        "usage: %s --sock PATH sign --op OPERATOR --csr FILE --out FILE\n"
        "       %s --sock PATH enroll-token --op OPERATOR --role ROLE --host FQDN [--ip IP] [--ttl SEC]\n"
        "       %s --sock PATH revoke-cert --op OPERATOR [--node ID] --serial HEX --fingerprint SHA256 --reason REASON [--confirm TOKEN]\n"
        "       %s --sock PATH ping\n"
        "  speaks the solariCtl operator-bridge line protocol over AF_UNIX\n",
        a0, a0, a0, a0);
}

int main(int argc, char **argv)
{
    const char *sock = NULL, *cmd = NULL;
    const char *op = NULL, *csrPath = NULL, *outPath = NULL;
    const char *role = NULL, *host = NULL, *ip = NULL, *ttl = NULL;
    const char *node = NULL, *serial = NULL, *fingerprint = NULL;
    const char *reason = NULL, *confirm = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sock") && i + 1 < argc)      sock = argv[++i];
        else if (!strcmp(argv[i], "--op") && i + 1 < argc)   op = argv[++i];
        else if (!strcmp(argv[i], "--csr") && i + 1 < argc)  csrPath = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)  outPath = argv[++i];
        else if (!strcmp(argv[i], "--role") && i + 1 < argc) role = argv[++i];
        else if (!strcmp(argv[i], "--host") && i + 1 < argc) host = argv[++i];
        else if (!strcmp(argv[i], "--ip") && i + 1 < argc) ip = argv[++i];
        else if (!strcmp(argv[i], "--ttl") && i + 1 < argc) ttl = argv[++i];
        else if (!strcmp(argv[i], "--node") && i + 1 < argc) node = argv[++i];
        else if (!strcmp(argv[i], "--serial") && i + 1 < argc) serial = argv[++i];
        else if (!strcmp(argv[i], "--fingerprint") && i + 1 < argc) fingerprint = argv[++i];
        else if (!strcmp(argv[i], "--reason") && i + 1 < argc) reason = argv[++i];
        else if (!strcmp(argv[i], "--confirm") && i + 1 < argc) confirm = argv[++i];
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]); return 0;
        }
        else if (argv[i][0] != '-' && !cmd) cmd = argv[i];
        else { usage(argv[0]); return 2; }
    }
    if (!sock || !cmd) { usage(argv[0]); return 2; }

    if (strcmp(cmd, "ping") == 0) {
        char reply[256];
        if (ctlRoundTrip(sock, "PING\n", reply, sizeof reply) != 0) {
            fprintf(stderr, "solariCtlClient: cannot reach %s\n", sock);
            return 3;
        }
        printf("%s\n", reply);
        return (strncmp(reply, "OK", 2) == 0) ? 0 : 1;
    }

    if (strcmp(cmd, "enroll-token") == 0) {
        char req[CTL_REQUEST_CAP], reply[1024];
        const char *values[] = {op, role, host, ip, ttl};
        size_t k, used;
        int n;
        if (!op || !role || !host) { usage(argv[0]); return 2; }
        for (k = 0; k < sizeof values / sizeof values[0]; ++k) {
            const char *p;
            if (!values[k]) continue;
            for (p = values[k]; *p; ++p) {
                if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' ||
                    *p == '=' || *p == '%') {
                    fprintf(stderr, "solariCtlClient: enrollment arguments contain a separator\n");
                    return 2;
                }
            }
        }
        n = snprintf(req, sizeof req, "ENROLL_TOKEN op=%s role=%s host=%s", op, role, host);
        if (n < 0 || (size_t)n >= sizeof req) return 2;
        used = (size_t)n;
        if (ip) {
            n = snprintf(req + used, sizeof req - used, " ip=%s", ip);
            if (n < 0 || (size_t)n >= sizeof req - used) return 2;
            used += (size_t)n;
        }
        if (ttl) {
            n = snprintf(req + used, sizeof req - used, " ttl=%s", ttl);
            if (n < 0 || (size_t)n >= sizeof req - used) return 2;
            used += (size_t)n;
        }
        if (used + 2 > sizeof req) return 2;
        req[used++] = '\n';
        req[used] = '\0';
        if (ctlRoundTrip(sock, req, reply, sizeof reply) != 0) {
            fprintf(stderr, "solariCtlClient: cannot reach %s\n", sock);
            return 3;
        }
        puts(reply);
        return strncmp(reply, "OK", 2) == 0 ? 0 : 1;
    }

    if (strcmp(cmd, "revoke-cert") == 0) {
        char req[1024], reply[1024];
        const char *values[] = {op, node, serial, fingerprint, reason, confirm};
        size_t k, used;
        int n;
        if (!op || !serial || !fingerprint || !reason) {
            usage(argv[0]); return 2;
        }
        for (k = 0; k < sizeof values / sizeof values[0]; ++k) {
            const char *p;
            if (!values[k]) continue;
            for (p = values[k]; *p; ++p) {
                if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' ||
                    *p == '=' || *p == '%') {
                    fprintf(stderr, "solariCtlClient: revocation arguments contain a separator\n");
                    return 2;
                }
            }
        }
        n = snprintf(req, sizeof req, "REVOKE_CERT op=%s", op);
        if (n < 0 || (size_t)n >= sizeof req) return 2;
        used = (size_t)n;
        if (node) {
            n = snprintf(req + used, sizeof req - used, " node=%s", node);
            if (n < 0 || (size_t)n >= sizeof req - used) return 2;
            used += (size_t)n;
        }
        n = snprintf(req + used, sizeof req - used,
                     " serial=%s fingerprint=%s reason=%s",
                     serial, fingerprint, reason);
        if (n < 0 || (size_t)n >= sizeof req - used) return 2;
        used += (size_t)n;
        if (confirm) {
            n = snprintf(req + used, sizeof req - used, " confirm=%s", confirm);
            if (n < 0 || (size_t)n >= sizeof req - used) return 2;
            used += (size_t)n;
        }
        if (used + 2 > sizeof req) return 2;
        req[used++] = '\n'; req[used] = '\0';
        if (ctlRoundTrip(sock, req, reply, sizeof reply) != 0) {
            fprintf(stderr, "solariCtlClient: cannot reach %s\n", sock);
            return 3;
        }
        puts(reply);
        return strncmp(reply, "OK", 2) == 0 ? 0 : 1;
    }

    if (strcmp(cmd, "sign") == 0) {
        char  *csr, *enc, *req, *reply;
        size_t csrLen = 0, n;
        FILE  *out;

        if (!op || !csrPath || !outPath) { usage(argv[0]); return 2; }
        csr = readFile(csrPath, &csrLen);
        if (!csr || csrLen == 0) {
            fprintf(stderr, "solariCtlClient: cannot read CSR %s\n", csrPath);
            free(csr);
            return 3;
        }
        enc   = (char *)malloc(CTL_MAX_LINE);
        req   = (char *)malloc(CTL_MAX_LINE);
        reply = (char *)malloc(CTL_MAX_LINE);
        if (!enc || !req || !reply) {
            free(csr); free(enc); free(req); free(reply);
            fprintf(stderr, "solariCtlClient: oom\n");
            return 3;
        }
        if (pctEncode(csr, enc, CTL_MAX_LINE) == 0) {
            fprintf(stderr, "solariCtlClient: CSR too large to encode\n");
            free(csr); free(enc); free(req); free(reply);
            return 3;
        }
        n = (size_t)snprintf(req, CTL_MAX_LINE, "SIGN op=%s csr=%s\n", op, enc);
        if (n >= CTL_REQUEST_CAP) {
            fprintf(stderr, "solariCtlClient: request too large\n");
            free(csr); free(enc); free(req); free(reply);
            return 3;
        }
        if (ctlRoundTrip(sock, req, reply, CTL_MAX_LINE) != 0) {
            fprintf(stderr, "solariCtlClient: cannot reach %s\n", sock);
            free(csr); free(enc); free(req); free(reply);
            return 3;
        }
        free(csr); free(enc); free(req);

        if (strncmp(reply, "OK cert=", 8) != 0) {
            fprintf(stderr, "solariCtlClient: CA SIGN failed: %s\n", reply);
            free(reply);
            return 1;
        }
        pctDecode(reply + 8);
        out = fopen(outPath, "w");
        if (!out || fputs(reply + 8, out) == EOF) {
            fprintf(stderr, "solariCtlClient: cannot write %s\n", outPath);
            if (out) fclose(out);
            free(reply);
            return 3;
        }
        fclose(out);
        free(reply);
        return 0;
    }

    usage(argv[0]);
    return 2;
}
