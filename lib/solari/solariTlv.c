/*
 * solariTlv.c - TLV writer/reader (§5.6, §6.3). All values big-endian.
 */
#include "solari/solariTlv.h"
#include "solariEndian.h"

#include <string.h>

#define TLV_HDR 4u   /* uint16 type + uint16 len */

/* -------------------------------------------------------------- writer */

void solariTlvWriterInit(solariTlvWriter *w, uint8_t *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->count = 0;
}

solariStatus solariTlvAppend(solariTlvWriter *w, uint16_t type,
                             const void *val, uint16_t len)
{
    if (!w || !w->buf || (len > 0 && !val)) {
        return ERR_INVALID_ARG;
    }
    if (w->len + TLV_HDR + (size_t)len > w->cap) {
        return ERR_BUFFER_FULL;
    }
    solariPutU16(w->buf + w->len,     type);
    solariPutU16(w->buf + w->len + 2, len);
    if (len > 0) {
        memcpy(w->buf + w->len + TLV_HDR, val, len);
    }
    w->len += TLV_HDR + (size_t)len;
    w->count++;
    return SOLARI_OK;
}

solariStatus solariTlvAppendU8(solariTlvWriter *w, uint16_t type, uint8_t v)
{
    return solariTlvAppend(w, type, &v, 1);
}

solariStatus solariTlvAppendU16(solariTlvWriter *w, uint16_t type, uint16_t v)
{
    uint8_t b[2];
    solariPutU16(b, v);
    return solariTlvAppend(w, type, b, sizeof b);
}

solariStatus solariTlvAppendU32(solariTlvWriter *w, uint16_t type, uint32_t v)
{
    uint8_t b[4];
    solariPutU32(b, v);
    return solariTlvAppend(w, type, b, sizeof b);
}

solariStatus solariTlvAppendU64(solariTlvWriter *w, uint16_t type, uint64_t v)
{
    uint8_t b[8];
    solariPutU64(b, v);
    return solariTlvAppend(w, type, b, sizeof b);
}

solariStatus solariTlvAppendStr(solariTlvWriter *w, uint16_t type, const char *s)
{
    size_t n;
    if (!s) {
        return ERR_INVALID_ARG;
    }
    n = strlen(s);
    if (n > 0xFFFFu) {
        return ERR_BUFFER_FULL;   /* does not fit a uint16 length */
    }
    return solariTlvAppend(w, type, s, (uint16_t)n);
}

/* -------------------------------------------------------------- reader */

void solariTlvReaderInit(solariTlvReader *r, const uint8_t *buf, size_t len)
{
    r->buf = buf;
    r->len = len;
    r->pos = 0;
}

solariStatus solariTlvNext(solariTlvReader *r, uint16_t *type,
                           const uint8_t **val, uint16_t *len)
{
    uint16_t t, l;
    if (!r) {
        return ERR_INVALID_ARG;
    }
    if (r->pos >= r->len) {
        return ERR_TLV_END;
    }
    if (r->pos + TLV_HDR > r->len) {
        return ERR_TLV_TRUNCATED;
    }
    t = solariGetU16(r->buf + r->pos);
    l = solariGetU16(r->buf + r->pos + 2);
    if (r->pos + TLV_HDR + (size_t)l > r->len) {
        return ERR_TLV_TRUNCATED;
    }
    if (type) *type = t;
    if (len)  *len = l;
    if (val)  *val = r->buf + r->pos + TLV_HDR;
    r->pos += TLV_HDR + (size_t)l;
    return SOLARI_OK;
}

solariStatus solariTlvReadU8(const uint8_t *val, uint16_t len, uint8_t *out)
{
    if (!val || !out || len != 1) return ERR_INVALID_ARG;
    *out = val[0];
    return SOLARI_OK;
}

solariStatus solariTlvReadU16(const uint8_t *val, uint16_t len, uint16_t *out)
{
    if (!val || !out || len != 2) return ERR_INVALID_ARG;
    *out = solariGetU16(val);
    return SOLARI_OK;
}

solariStatus solariTlvReadU32(const uint8_t *val, uint16_t len, uint32_t *out)
{
    if (!val || !out || len != 4) return ERR_INVALID_ARG;
    *out = solariGetU32(val);
    return SOLARI_OK;
}

solariStatus solariTlvReadU64(const uint8_t *val, uint16_t len, uint64_t *out)
{
    if (!val || !out || len != 8) return ERR_INVALID_ARG;
    *out = solariGetU64(val);
    return SOLARI_OK;
}
