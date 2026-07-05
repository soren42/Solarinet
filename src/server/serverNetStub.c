/*
 * serverNetStub.c - no-I/O transport stubs for dependency-free server builds.
 *
 * SOLARI_WITH_IO=OFF omits solariNet.c from libsolari. Server binaries still
 * compile in this configuration for pure logic and DB tests, but transport
 * entry points report ERR_PLATFORM if accidentally reached.
 */
#include "solari/solariNet.h"

#include <stddef.h>

solariStatus solariConnOpen(const solariConnOpts *opts, solariConn **out)
{
    (void)opts;
    if (out) *out = NULL;
    return ERR_PLATFORM;
}

solariStatus solariConnListen(const solariConnOpts *opts, solariConn **out)
{
    (void)opts;
    if (out) *out = NULL;
    return ERR_PLATFORM;
}

solariStatus solariConnSend(solariConn *c, const uint8_t *frame, size_t len)
{
    (void)c;
    (void)frame;
    (void)len;
    return ERR_PLATFORM;
}

solariStatus solariConnRecv(solariConn *c, const uint8_t **frame, size_t *len)
{
    (void)c;
    if (frame) *frame = NULL;
    if (len) *len = 0;
    return ERR_PLATFORM;
}

solariStatus solariConnPeerCn(solariConn *c, char *buf, size_t cap)
{
    (void)c;
    if (buf && cap) buf[0] = '\0';
    return ERR_PLATFORM;
}

void solariConnClose(solariConn *c)
{
    (void)c;
}
