#include "server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int readFile(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f) return -1;
    n = fread(buf, 1, cap - 1, f);
    if (ferror(f) || !feof(f)) {
        fclose(f);
        return -1;
    }
    buf[n] = '\0';
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    serverConfig cfg;
    serverContext ctx;
    serverCtl *ctl = NULL;
    char csr[8192];
    char cert[8192];
    char leafFp[65], leafSerial[41], issuerFp[65];
    char caFp[65];
    FILE *out;
    int writeFailed;
    solariStatus st;

    if (argc != 8) {
        fprintf(stderr, "usage: %s CA.pem CA.key CSR.pem EXPECTED_CN EXPECTED_FQDN OUT.pem SOCKET\n",
                argv[0]);
        return 2;
    }
    if (readFile(argv[3], csr, sizeof csr) != 0) {
        fprintf(stderr, "cannot read CSR: %s\n", argv[3]);
        return 2;
    }
    memset(&cfg, 0, sizeof cfg);
    memset(&ctx, 0, sizeof ctx);
    snprintf(cfg.caCertFile, sizeof cfg.caCertFile, "%s", argv[1]);
    snprintf(cfg.caKeyFile, sizeof cfg.caKeyFile, "%s", argv[2]);
    snprintf(cfg.ctlSocket, sizeof cfg.ctlSocket, "%s", argv[7]);
    if (serverCtlOpen(&cfg, &ctx, &ctl) != SOLARI_OK) {
        fprintf(stderr, "serverCtlOpen failed\n");
        return 2;
    }
    if (serverCtlCaFingerprint(ctl, caFp) != SOLARI_OK || strlen(caFp) != 64) {
        fprintf(stderr, "configured CA fingerprint unavailable\n");
        serverCtlClose(ctl);
        return 2;
    }
    st = serverCtlSignCsr(ctl, csr, argv[4], argv[5], cert, sizeof cert,
                          leafFp, sizeof leafFp, leafSerial, sizeof leafSerial,
                          issuerFp, sizeof issuerFp);
    serverCtlClose(ctl);
    if (st != SOLARI_OK) {
        fprintf(stderr, "enrollment signing rejected with status %d\n", (int)st);
        return 1;
    }
    if (strcmp(issuerFp, caFp) != 0) {
        fprintf(stderr, "issued certificate CA identity mismatch\n");
        return 1;
    }
    out = fopen(argv[6], "wb");
    if (!out) {
        fprintf(stderr, "cannot write issued certificate: %s\n", argv[6]);
        return 2;
    }
    writeFailed = fputs(cert, out) == EOF;
    if (fclose(out) != 0) writeFailed = 1;
    if (writeFailed) {
        fprintf(stderr, "cannot write issued certificate: %s\n", argv[6]);
        return 2;
    }
    printf("caFp=%s fp=%s serial=%s\n", caFp, leafFp, leafSerial);
    return 0;
}
