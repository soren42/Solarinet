/* Shared, allocation-free Solari Panel wire codec. */
#include "protocol.h"

#include <string.h>

/* Purpose: read an unsigned little-endian 16-bit value. Input: two bytes. Output: value. */
static uint16_t readLe16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }
/* Purpose: read an unsigned little-endian 32-bit value. Input: four bytes. Output: value. */
static uint32_t readLe32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
/* Purpose: write an unsigned little-endian 16-bit value. Input: destination/value. Output: bytes written. */
static void writeLe16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
/* Purpose: write an unsigned little-endian 32-bit value. Input: destination/value. Output: bytes written. */
static void writeLe32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* Purpose: copy a display name into its fixed wire field. Input: source/field width. Output: uppercase safe, NUL-padded field. */
static void writeName(uint8_t *out, const char *in, size_t width) {
  size_t i; int truncated = 0;
  memset(out, 0, width);
  for (i = 0; i < width; ++i) {
    unsigned char c = (unsigned char)in[i];
    if (c == '\0') break;
    if (c >= 'a' && c <= 'z') c = (unsigned char)(c - ('a' - 'A'));
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '.' || c == ':' || c == '-')) c = '.';
    out[i] = c;
  }
  if (i == width && in[i] != '\0') truncated = 1;
  if (truncated && width != 0u) out[width - 1u] = '.';
}

/* Purpose: copy a fixed wire name into a terminated in-memory string. Input: wire field/width. Output: sanitized terminated name. */
static void readName(char *out, const uint8_t *in, size_t width) {
  size_t i;
  for (i = 0; i < width && in[i] != 0u; ++i) out[i] = (char)in[i];
  out[i] = '\0';
}

/* Purpose: calculate protocol CRC-16/CCITT. Input: byte sequence. Output: CRC initialized to 0xffff. */
uint16_t panelCrc16(const uint8_t *data, size_t len) {
  uint16_t crc = 0xffffu; size_t i; unsigned int bit;
  for (i = 0; i < len; ++i) { crc ^= (uint16_t)data[i] << 8; for (bit = 0; bit < 8u; ++bit) crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1); }
  return crc;
}

/* Purpose: compare wrapping 16-bit snapshot sequence numbers. Input: candidate/last applied. Output: nonzero only for RFC1982-newer candidate. */
int panelSeqNewer(uint16_t candidate, uint16_t lastApplied) {
  uint16_t difference = (uint16_t)(candidate - lastApplied);
  return difference != 0u && difference < 0x8000u;
}

/* Purpose: encode one complete protocol frame. Input: type/payload/output buffer. Output: frame length, or zero on invalid capacity/input. */
size_t panelEncodeFrame(uint8_t type, const uint8_t *payload, size_t payloadLen, uint8_t *out, size_t outCap) {
  size_t total = PANEL_HDR_SIZE + payloadLen + PANEL_CRC_SIZE; uint16_t crc;
  if (payloadLen > PANEL_MAX_PAYLOAD || out == NULL || outCap < total || (payloadLen != 0u && payload == NULL)) return 0u;
  out[0] = PANEL_MAGIC0; out[1] = PANEL_MAGIC1; out[2] = PANEL_PROTO_VERSION; out[3] = type; writeLe16(out + 4, (uint16_t)payloadLen);
  if (payloadLen != 0u) memcpy(out + PANEL_HDR_SIZE, payload, payloadLen);
  crc = panelCrc16(out + 2, 4u + payloadLen); writeLe16(out + PANEL_HDR_SIZE + payloadLen, crc); return total;
}

/* Purpose: encode canonical snapshot payload. Input: snapshot/output buffer. Output: payload size, or zero on invalid capacity/counts. */
size_t panelEncodeSnapshot(const PanelSnapshot *s, uint8_t *p, size_t cap) {
  size_t i, off, need;
  if (s == NULL || p == NULL || s->poolCount > PANEL_MAX_POOLS || s->systemCount > PANEL_MAX_SYSTEMS) return 0u;
  need = PANEL_GLOBALS_SIZE + PANEL_POOL_STRIDE * (size_t)s->poolCount + PANEL_SYS_STRIDE * (size_t)s->systemCount + (s->hasTopAlert ? PANEL_ALERT_SIZE : 0u);
  if (cap < need) return 0u;
  memset(p, 0, need);
  writeLe32(p, s->ts); writeLe16(p + 4, s->seq); writeLe16(p + 6, s->score); p[8] = s->alarmActive; p[9] = s->dataStale; memcpy(p + 10, s->stateRoll, 5u); memcpy(p + 15, s->alertCounts, 3u); p[18] = s->meanLoadPct; p[19] = s->poolCount; p[20] = s->systemCount; p[21] = s->hasTopAlert ? 1u : 0u; writeLe32(p + 22, s->rxKbps); writeLe32(p + 26, s->txKbps); writeLe16(p + 30, s->rttTenthMs); writeLe16(p + 32, s->lossPermille);
  off = PANEL_GLOBALS_SIZE;
  for (i = 0; i < s->poolCount; ++i, off += PANEL_POOL_STRIDE) { writeName(p + off, s->pools[i].name, PANEL_NAME_POOL); p[off+8] = s->pools[i].tier; p[off+9] = s->pools[i].ok; p[off+10] = s->pools[i].degraded; p[off+11] = s->pools[i].down; p[off+12] = s->pools[i].unknown; p[off+13] = s->pools[i].maint; p[off+14] = s->pools[i].total; p[off+15] = s->pools[i].loadPct; }
  for (i = 0; i < s->systemCount; ++i, off += PANEL_SYS_STRIDE) { writeName(p + off, s->systems[i].name, PANEL_NAME_SYS); p[off+12] = s->systems[i].state; p[off+13] = s->systems[i].pool; p[off+14] = s->systems[i].tier; p[off+15] = s->systems[i].loadPct; }
  if (s->hasTopAlert) { writeLe32(p + off, s->topAlert.alertId); writeLe32(p + off + 4, s->topAlert.episodeId); p[off+8] = s->topAlert.severity; writeName(p + off + 12, s->topAlert.subject, PANEL_ALERT_SUBJ); writeName(p + off + 36, s->topAlert.detail, PANEL_ALERT_DETAIL); }
  return need;
}

/* Purpose: decode canonical snapshot payload. Input: complete payload. Output: filled snapshot or -1 for malformed layout. */
int panelDecodeSnapshot(const uint8_t *p, size_t len, PanelSnapshot *s) {
  size_t i, off, need; uint8_t pools, systems, alert;
  if (p == NULL || s == NULL || len < PANEL_GLOBALS_SIZE) return -1;
  pools = p[19]; systems = p[20]; alert = p[21]; if (pools > PANEL_MAX_POOLS || systems > PANEL_MAX_SYSTEMS || alert > 1u) return -1;
  need = PANEL_GLOBALS_SIZE + PANEL_POOL_STRIDE * (size_t)pools + PANEL_SYS_STRIDE * (size_t)systems + (alert ? PANEL_ALERT_SIZE : 0u); if (len < need) return -1;
  memset(s, 0, sizeof(*s)); s->ts = readLe32(p); s->seq = readLe16(p+4); s->score = readLe16(p+6); s->alarmActive=p[8]; s->dataStale=p[9]; memcpy(s->stateRoll,p+10,5u); memcpy(s->alertCounts,p+15,3u); s->meanLoadPct=p[18]; s->poolCount=pools; s->systemCount=systems; s->hasTopAlert=alert; s->rxKbps=readLe32(p+22); s->txKbps=readLe32(p+26); s->rttTenthMs=readLe16(p+30); s->lossPermille=readLe16(p+32); off=PANEL_GLOBALS_SIZE;
  for(i=0;i<pools;++i,off+=PANEL_POOL_STRIDE){ readName(s->pools[i].name,p+off,PANEL_NAME_POOL); s->pools[i].tier=p[off+8]; s->pools[i].ok=p[off+9]; s->pools[i].degraded=p[off+10]; s->pools[i].down=p[off+11]; s->pools[i].unknown=p[off+12]; s->pools[i].maint=p[off+13]; s->pools[i].total=p[off+14]; s->pools[i].loadPct=p[off+15]; }
  for(i=0;i<systems;++i,off+=PANEL_SYS_STRIDE){ readName(s->systems[i].name,p+off,PANEL_NAME_SYS); s->systems[i].state=p[off+12]; s->systems[i].pool=p[off+13]; s->systems[i].tier=p[off+14]; s->systems[i].loadPct=p[off+15]; }
  if(alert){ s->topAlert.alertId=readLe32(p+off); s->topAlert.episodeId=readLe32(p+off+4); s->topAlert.severity=p[off+8]; readName(s->topAlert.subject,p+off+12,PANEL_ALERT_SUBJ); readName(s->topAlert.detail,p+off+36,PANEL_ALERT_DETAIL); } return 0;
}

/* Purpose: keep a possible overlapping magic suffix after parser rejection. Input: parser/start offset. Output: parser buffer repositioned. */
static void parserResync(PanelParser *p, size_t start) { size_t i, remain = 0u; for (i=start;i+1u<p->have;++i) if(p->buf[i]==PANEL_MAGIC0 && p->buf[i+1u]==PANEL_MAGIC1){ remain=p->have-i; memmove(p->buf,p->buf+i,remain); break; } if(remain==0u && p->have>start && p->buf[p->have-1u]==PANEL_MAGIC0){p->buf[0]=PANEL_MAGIC0;remain=1u;} p->have=remain; p->resyncs++; }
/* Purpose: identify defined frame types. Input: type byte. Output: nonzero when dispatchable. */
static int knownType(uint8_t type) { return type >= PANEL_FT_SNAPSHOT && type <= PANEL_FT_HELLOREQ ? 1 : (type >= PANEL_FT_HELLO && type <= PANEL_FT_LOG ? 1 : 0); }
/* Purpose: initialize frame parser state. Input: parser. Output: zeroed parser. */
void panelParserInit(PanelParser *p) { if (p != NULL) memset(p, 0, sizeof(*p)); }
/* Purpose: consume serial bytes and dispatch valid frames. Input: bytes/time/callback. Output: callback invocations; malformed frames are discarded. */
void panelParserFeed(PanelParser *p, const uint8_t *bytes, size_t n, uint32_t nowMs, PanelFrameCb cb, void *user) {
  size_t i; if(p==NULL || (n!=0u && bytes==NULL)) return;
  if(p->have != 0u && (uint32_t)(nowMs-p->lastByteMs)>PANEL_FRAME_TIMEOUT_MS) parserResync(p,1u);
  for(i=0;i<n;++i){
    p->lastByteMs=nowMs;
    if(p->have==0u){ if(bytes[i]==PANEL_MAGIC0)p->buf[p->have++]=bytes[i]; }
    else if(p->have==1u){ if(bytes[i]==PANEL_MAGIC1)p->buf[p->have++]=bytes[i]; else if(bytes[i]!=PANEL_MAGIC0)p->have=0u; }
    else { p->buf[p->have++]=bytes[i]; }
    for(;;){
      size_t plen, total, remain;
      if(p->have < PANEL_HDR_SIZE) break;
      plen=readLe16(p->buf+4);
      if(plen>PANEL_MAX_PAYLOAD){ parserResync(p,1u); continue; }
      total=PANEL_HDR_SIZE+plen+PANEL_CRC_SIZE;
      if(p->have<total) break;
      if(readLe16(p->buf+PANEL_HDR_SIZE+plen)!=panelCrc16(p->buf+2,4u+plen)){
        p->crcErrors++; parserResync(p,1u); continue;
      }
      if(p->buf[2]==PANEL_PROTO_VERSION && knownType(p->buf[3]) && cb!=NULL) cb(p->buf[3],p->buf+PANEL_HDR_SIZE,plen,user);
      remain=p->have-total;
      if(remain!=0u) memmove(p->buf,p->buf+total,remain);
      p->have=remain;
    }
  }
}
