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
 * The percent-encoding mirrors solariCtl.c's ctlPctEncode/ctlPctDecode exactly
 * (bare-token set [A-Za-z0-9._:/@-], everything else %XX): values must not
 * contain the protocol's separators (space/tab/newline), and PEM bodies carry
 * '+', '=', and newlines, so full encoding is required on both directions.
 * Exit codes: 0 ok, 1 ERR reply / bad reply, 2 usage, 3 I/O or socket failure.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define CTL_MAX_LINE (256 * 1024)   /* generous: CSR/cert PEMs are a few KB */

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
        if (w <= 0) { close(fd); return -1; }
        off += (size_t)w;
    }
    off = 0;
    for (;;) {
        ssize_t r;
        char   *nl;
        if (off + 1 >= cap) { close(fd); return -1; }
        r = read(fd, reply + off, cap - 1 - off);
        if (r < 0) { close(fd); return -1; }
        if (r == 0) break;
        off += (size_t)r;
        reply[off] = '\0';
        nl = strchr(reply, '\n');
        if (nl) { *nl = '\0'; break; }
    }
    close(fd);
    return (off > 0) ? 0 : -1;
}

static void usage(const char *a0)
{
    fprintf(stderr,
        "usage: %s --sock PATH sign --op OPERATOR --csr FILE --out FILE\n"
        "       %s --sock PATH ping\n"
        "  speaks the solariCtl operator-bridge line protocol over AF_UNIX\n",
        a0, a0);
}

int main(int argc, char **argv)
{
    const char *sock = NULL, *cmd = NULL;
    const char *op = NULL, *csrPath = NULL, *outPath = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--sock") && i + 1 < argc)      sock = argv[++i];
        else if (!strcmp(argv[i], "--op") && i + 1 < argc)   op = argv[++i];
        else if (!strcmp(argv[i], "--csr") && i + 1 < argc)  csrPath = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)  outPath = argv[++i];
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
        if (n >= CTL_MAX_LINE) {
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
