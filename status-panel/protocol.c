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

/* Purpose: encode a CONTROL payload at its fixed protocol offsets. Input: command fields/output buffer. Output: payload size, or zero for invalid capacity. */
size_t panelEncodeControl(uint32_t cmdId, uint8_t kind, uint8_t arg,
                          uint8_t *payload, size_t cap) {
  if (payload == NULL || cap < PANEL_CONTROL_SIZE) return 0u;
  memset(payload, 0, PANEL_CONTROL_SIZE);
  writeLe32(payload, cmdId);
  payload[4] = kind;
  payload[5] = arg;
  return PANEL_CONTROL_SIZE;
}

/* Purpose: decode a CONTROL payload at its fixed protocol offsets. Input: complete payload/output fields. Output: zero success, -1 for malformed input. */
int panelDecodeControl(const uint8_t *payload, size_t len, uint32_t *cmdId,
                       uint8_t *kind, uint8_t *arg) {
  if (payload == NULL || cmdId == NULL || kind == NULL || arg == NULL || len < PANEL_CONTROL_SIZE) return -1;
  *cmdId = readLe32(payload);
  *kind = payload[4];
  *arg = payload[5];
  return 0;
}

/* Purpose: encode a STATE payload at its fixed protocol offsets. Input: state fields/output buffer. Output: payload size, or zero for invalid capacity. */
size_t panelEncodeState(uint8_t theme, uint8_t screen, uint8_t brightnessPct,
                        uint8_t autoBright, uint8_t sleeping, uint8_t alarmArmed,
                        uint8_t alarmAcked, uint8_t dwellSec, uint32_t ackedEpisodeId,
                        uint32_t lastCmdId, uint8_t *payload, size_t cap) {
  if (payload == NULL || cap < PANEL_STATE_SIZE) return 0u;
  memset(payload, 0, PANEL_STATE_SIZE);
  payload[0] = theme;
  payload[1] = screen;
  payload[2] = brightnessPct;
  payload[3] = autoBright;
  payload[4] = sleeping;
  payload[5] = alarmArmed;
  payload[6] = alarmAcked;
  payload[7] = dwellSec;
  writeLe32(payload + 8, ackedEpisodeId);
  writeLe32(payload + 12, lastCmdId);
  return PANEL_STATE_SIZE;
}

/* Purpose: decode a STATE payload at its fixed protocol offsets. Input: complete payload/output fields. Output: zero success, -1 for malformed input. */
int panelDecodeState(const uint8_t *payload, size_t len, uint8_t *theme,
                     uint8_t *screen, uint8_t *brightnessPct,
                     uint8_t *autoBright, uint8_t *sleeping,
                     uint8_t *alarmArmed, uint8_t *alarmAcked,
                     uint8_t *dwellSec, uint32_t *ackedEpisodeId,
                     uint32_t *lastCmdId) {
  if (payload == NULL || theme == NULL || screen == NULL || brightnessPct == NULL || autoBright == NULL || sleeping == NULL || alarmArmed == NULL || alarmAcked == NULL || dwellSec == NULL || ackedEpisodeId == NULL || lastCmdId == NULL || len < PANEL_STATE_SIZE) return -1;
  *theme = payload[0];
  *screen = payload[1];
  *brightnessPct = payload[2];
  *autoBright = payload[3];
  *sleeping = payload[4];
  *alarmArmed = payload[5];
  *alarmAcked = payload[6];
  *dwellSec = payload[7];
  *ackedEpisodeId = readLe32(payload + 8);
  *lastCmdId = readLe32(payload + 12);
  return 0;
}

/* Purpose: encode a CONFIG payload at its fixed protocol offsets (CONTRACT-V2 §1). Input: per-screen config/flags/output buffer. Output: payload size, or zero for invalid capacity. */
size_t panelEncodeConfig(const uint8_t screenCfg[PANEL_SCREEN_SLOTS], uint8_t flags,
                         uint8_t *payload, size_t cap) {
  if (screenCfg == NULL || payload == NULL || cap < PANEL_CONFIG_SIZE) return 0u;
  memset(payload, 0, PANEL_CONFIG_SIZE);
  memcpy(payload, screenCfg, 12u);
  payload[12] = flags;
  memcpy(payload + PANEL_CONFIG_SIZE_V1, screenCfg + 12u, PANEL_SCREEN_SLOTS - 12u);
  return PANEL_CONFIG_SIZE;
}

/* Purpose: decode a CONFIG payload at its fixed protocol offsets; a 16-byte pre-v2 payload defaults slots 12-15 to enabled, 1x. Input: complete payload/output fields. Output: zero success, -1 for malformed input. */
int panelDecodeConfig(const uint8_t *payload, size_t len,
                      uint8_t screenCfg[PANEL_SCREEN_SLOTS], uint8_t *flags) {
  size_t i;
  if (payload == NULL || screenCfg == NULL || flags == NULL || len < PANEL_CONFIG_SIZE_V1) return -1;
  for (i = 0u; i < 12u; ++i) if ((payload[i] & 0xf0u) != 0u) return -1;
  if (payload[13] != 0u || payload[14] != 0u || payload[15] != 0u) return -1;
  if (len >= PANEL_CONFIG_SIZE) {
    for (i = PANEL_CONFIG_SIZE_V1; i < PANEL_CONFIG_SIZE; ++i) if ((payload[i] & 0xf0u) != 0u) return -1;
  }
  memcpy(screenCfg, payload, 12u);
  if (len >= PANEL_CONFIG_SIZE) memcpy(screenCfg + 12u, payload + PANEL_CONFIG_SIZE_V1, PANEL_SCREEN_SLOTS - 12u);
  else memset(screenCfg + 12u, 0x05, PANEL_SCREEN_SLOTS - 12u);
  *flags = payload[12];
  return 0;
}

/* Purpose: encode a PROVISION TLV at its fixed protocol offsets. Input: item/generation/offset/data/output buffer; data must not overlap payload. Output: payload size, or zero for invalid capacity/input. */
size_t panelEncodeProvision(uint8_t itemId, uint8_t generation,
                            uint16_t offset, const uint8_t *data,
                            uint16_t dataLen, uint8_t *payload, size_t cap) {
  size_t total = PANEL_PROV_HDR_SIZE + dataLen;
  if (payload == NULL || cap < total || total > PANEL_MAX_PAYLOAD || (dataLen != 0u && data == NULL)) return 0u;
  payload[0] = itemId;
  payload[1] = generation;
  writeLe16(payload + 2, offset);
  writeLe16(payload + 4, dataLen);
  if (dataLen != 0u) memcpy(payload + PANEL_PROV_HDR_SIZE, data, dataLen);
  return total;
}

/* Purpose: decode a PROVISION TLV at its fixed protocol offsets. Input: complete payload/output fields; data aliases the payload. Output: zero success, -1 for malformed input (short header, payload over PANEL_MAX_PAYLOAD, or declared len not matching the payload exactly). */
int panelDecodeProvision(const uint8_t *payload, size_t len, uint8_t *itemId,
                         uint8_t *generation, uint16_t *offset,
                         const uint8_t **data, uint16_t *dataLen) {
  if (payload == NULL || itemId == NULL || generation == NULL || offset == NULL || data == NULL || dataLen == NULL || len < PANEL_PROV_HDR_SIZE || len > PANEL_MAX_PAYLOAD) return -1;
  if (readLe16(payload + 4) != len - PANEL_PROV_HDR_SIZE) return -1;
  *itemId = payload[0];
  *generation = payload[1];
  *offset = readLe16(payload + 2);
  *dataLen = readLe16(payload + 4);
  *data = payload + PANEL_PROV_HDR_SIZE;
  return 0;
}

/* Purpose: encode a PROVACK payload at its fixed protocol offsets. Input: item/status/generation/nextOffset/output buffer. Output: payload size, or zero for invalid capacity. */
size_t panelEncodeProvAck(uint8_t itemId, uint8_t status, uint8_t generation,
                          uint16_t nextOffset, uint8_t *payload, size_t cap) {
  if (payload == NULL || cap < PANEL_PROVACK_SIZE) return 0u;
  payload[0] = itemId;
  payload[1] = status;
  payload[2] = generation;
  writeLe16(payload + 3, nextOffset);
  return PANEL_PROVACK_SIZE;
}

/* Purpose: decode a PROVACK payload at its fixed protocol offsets. Input: complete payload/output fields; trailing bytes beyond PANEL_PROVACK_SIZE are tolerated (same-version additive extension, matching the CONTROL/STATE convention). Output: zero success, -1 for malformed input. */
int panelDecodeProvAck(const uint8_t *payload, size_t len, uint8_t *itemId,
                       uint8_t *status, uint8_t *generation,
                       uint16_t *nextOffset) {
  if (payload == NULL || itemId == NULL || status == NULL || generation == NULL || nextOffset == NULL || len < PANEL_PROVACK_SIZE) return -1;
  *itemId = payload[0];
  *status = payload[1];
  *generation = payload[2];
  *nextOffset = readLe16(payload + 3);
  return 0;
}

/* Purpose: encode a PANEL_EV_ACK EVENT payload with its episode coverage (D16). Input: coverage bits/episode ids/output buffer. Output: payload size, or zero for invalid capacity. */
size_t panelEncodeAckEvent(uint8_t coverage, uint32_t serverEpisode,
                           uint32_t powerEpisode, uint8_t *payload, size_t cap) {
  if (payload == NULL || cap < PANEL_ACKEV_SIZE) return 0u;
  payload[0] = PANEL_EV_ACK;
  payload[1] = coverage;
  writeLe32(payload + 2, (coverage & PANEL_ACKEV_SERVER) ? serverEpisode : 0u);
  writeLe32(payload + 6, (coverage & PANEL_ACKEV_POWER) ? powerEpisode : 0u);
  return PANEL_ACKEV_SIZE;
}

/* Purpose: decode a PANEL_EV_ACK EVENT payload; trailing bytes beyond PANEL_ACKEV_SIZE are tolerated (additive-extension convention). Input: complete payload/output fields. Output: zero success, -1 for malformed input or a non-ACK kind. */
int panelDecodeAckEvent(const uint8_t *payload, size_t len, uint8_t *coverage,
                        uint32_t *serverEpisode, uint32_t *powerEpisode) {
  if (payload == NULL || coverage == NULL || serverEpisode == NULL || powerEpisode == NULL || len < PANEL_ACKEV_SIZE || payload[0] != PANEL_EV_ACK) return -1;
  *coverage = payload[1];
  *serverEpisode = readLe32(payload + 2);
  *powerEpisode = readLe32(payload + 6);
  return 0;
}

/* Purpose: encode one complete protocol frame. Input: type/payload/output buffer. Output: frame length, or zero on invalid capacity/input. */
size_t panelEncodeFrame(uint8_t type, const uint8_t *payload, size_t payloadLen, uint8_t *out, size_t outCap) {
  size_t total = PANEL_HDR_SIZE + payloadLen + PANEL_CRC_SIZE; uint16_t crc;
  if (payloadLen > PANEL_MAX_PAYLOAD || out == NULL || outCap < total || (payloadLen != 0u && payload == NULL)) return 0u;
  out[0] = PANEL_MAGIC0; out[1] = PANEL_MAGIC1; out[2] = PANEL_PROTO_VERSION; out[3] = type; writeLe16(out + 4, (uint16_t)payloadLen);
  if (payloadLen != 0u) memcpy(out + PANEL_HDR_SIZE, payload, payloadLen);
  crc = panelCrc16(out + 2, 4u + payloadLen); writeLe16(out + PANEL_HDR_SIZE + payloadLen, crc); return total;
}

/* Purpose: payload bytes the CONTRACT-V2 TLVs need. Input: snapshot. Output: byte count. */
static size_t panelExtSize(const PanelSnapshot *s) {
  size_t n = 0u;
  if (s->hasLinks) n += 3u + 1u + PANEL_LINK_STRIDE * (size_t)s->linkCount;
  if (s->hasSysx) n += 3u + 1u + PANEL_SYSX_STRIDE * (size_t)s->systemCount;
  if (s->hasAlerts) n += 3u + 2u + PANEL_FAULT_CODES + PANEL_ALERTX_STRIDE * (size_t)s->alertCount;
  if (s->hasProbes) n += 3u + 1u + PANEL_PROBE_STRIDE * (size_t)s->probeCount;
  if (s->popCount != 0u) n += 3u + 1u + (size_t)s->popCount;
  if (s->hasHeat) n += 3u + 2u * PANEL_HEAT_LEN;
  if (s->hasTotals) n += 3u + 16u;
  return n;
}

/* Purpose: start one TLV. Input: output cursor, tag, value length. Output: value cursor. */
static uint8_t *tlvStart(uint8_t *o, uint8_t tag, size_t len) {
  o[0] = tag; writeLe16(o + 1, (uint16_t)len); return o + 3;
}

/* Purpose: write the CONTRACT-V2 TLVs; panelExtSize has already bounded them. Input: snapshot/output cursor. Output: none. */
static void panelExtWrite(const PanelSnapshot *s, uint8_t *o) {
  size_t i;
  if (s->hasLinks) {
    o = tlvStart(o, PANEL_TLV_LINKS, 1u + PANEL_LINK_STRIDE * (size_t)s->linkCount);
    *o++ = s->linkCount;
    for (i = 0u; i < s->linkCount; ++i, o += PANEL_LINK_STRIDE) {
      writeName(o, s->links[i].name, PANEL_NAME_SYS);
      o[12] = s->links[i].kind; o[13] = s->links[i].state; o[14] = s->links[i].parent; o[15] = 0u;
      writeLe32(o + 16, s->links[i].upKbps); writeLe32(o + 20, s->links[i].downKbps);
    }
  }
  if (s->hasSysx) {
    o = tlvStart(o, PANEL_TLV_SYSX, 1u + PANEL_SYSX_STRIDE * (size_t)s->systemCount);
    *o++ = s->systemCount;
    for (i = 0u; i < s->systemCount; ++i, o += PANEL_SYSX_STRIDE) {
      o[0] = s->sysx[i].memPct; o[1] = s->sysx[i].diskPct; o[2] = s->sysx[i].faults; o[3] = 0u;
      writeLe32(o + 4, s->sysx[i].rxKbps); writeLe32(o + 8, s->sysx[i].txKbps);
    }
  }
  if (s->hasAlerts) {
    o = tlvStart(o, PANEL_TLV_ALERTS, 2u + PANEL_FAULT_CODES + PANEL_ALERTX_STRIDE * (size_t)s->alertCount);
    *o++ = s->alertTotal;
    memcpy(o, s->faultCounts, PANEL_FAULT_CODES); o += PANEL_FAULT_CODES;
    *o++ = s->alertCount;
    for (i = 0u; i < s->alertCount; ++i, o += PANEL_ALERTX_STRIDE) {
      o[0] = s->alerts[i].severity; o[1] = s->alerts[i].code; writeLe16(o + 2, s->alerts[i].ageMin);
      writeName(o + 4, s->alerts[i].host, PANEL_NAME_SYS);
      writeName(o + 16, s->alerts[i].msg, PANEL_ALERT_MSG);
    }
  }
  if (s->hasProbes) {
    o = tlvStart(o, PANEL_TLV_PROBES, 1u + PANEL_PROBE_STRIDE * (size_t)s->probeCount);
    *o++ = s->probeCount;
    for (i = 0u; i < s->probeCount; ++i, o += PANEL_PROBE_STRIDE) {
      writeName(o, s->probes[i].name, PANEL_NAME_PROBE);
      writeLe16(o + 8, s->probes[i].rttTenthMs); writeLe16(o + 10, s->probes[i].lossPermille);
      o[12] = s->probes[i].state; o[13] = 0u;
    }
  }
  if (s->popCount != 0u) {
    o = tlvStart(o, PANEL_TLV_POP, 1u + (size_t)s->popCount);
    *o++ = s->popCount;
    memcpy(o, s->pop, s->popCount); o += s->popCount;
  }
  if (s->hasHeat) {
    o = tlvStart(o, PANEL_TLV_HEAT, 2u * PANEL_HEAT_LEN);
    memcpy(o, s->heatWarn, PANEL_HEAT_LEN); memcpy(o + PANEL_HEAT_LEN, s->heatCrit, PANEL_HEAT_LEN);
    o += 2u * PANEL_HEAT_LEN;
  }
  if (s->hasTotals) {
    o = tlvStart(o, PANEL_TLV_TOTALS, 16u);
    writeLe32(o, s->wanUpKbps); writeLe32(o + 4, s->wanDownKbps);
    writeLe32(o + 8, s->lanUpKbps); writeLe32(o + 12, s->lanDownKbps);
  }
}

/* Purpose: decode one known TLV into the snapshot, leaving its has* flag 0 when malformed. Input: tag/value/length/snapshot. Output: none. */
static void panelExtValue(uint8_t tag, const uint8_t *v, size_t n, PanelSnapshot *s) {
  size_t i, count;
  switch (tag) {
    case PANEL_TLV_LINKS:
      if (n < 1u || (count = v[0]) > PANEL_MAX_LINKS || n < 1u + count * PANEL_LINK_STRIDE) return;
      for (i = 0u, v += 1; i < count; ++i, v += PANEL_LINK_STRIDE) {
        readName(s->links[i].name, v, PANEL_NAME_SYS);
        s->links[i].kind = v[12]; s->links[i].state = v[13]; s->links[i].parent = v[14];
        s->links[i].upKbps = readLe32(v + 16); s->links[i].downKbps = readLe32(v + 20);
      }
      s->linkCount = (uint8_t)count; s->hasLinks = 1u;
      return;
    case PANEL_TLV_SYSX:
      if (n < 1u || (count = v[0]) != s->systemCount || n < 1u + count * PANEL_SYSX_STRIDE) return;
      for (i = 0u, v += 1; i < count; ++i, v += PANEL_SYSX_STRIDE) {
        s->sysx[i].memPct = v[0]; s->sysx[i].diskPct = v[1]; s->sysx[i].faults = v[2];
        s->sysx[i].rxKbps = readLe32(v + 4); s->sysx[i].txKbps = readLe32(v + 8);
      }
      s->hasSysx = 1u;
      return;
    case PANEL_TLV_ALERTS:
      if (n < 2u + PANEL_FAULT_CODES || (count = v[1u + PANEL_FAULT_CODES]) > PANEL_MAX_ALERTS ||
          n < 2u + PANEL_FAULT_CODES + count * PANEL_ALERTX_STRIDE) return;
      s->alertTotal = v[0];
      memcpy(s->faultCounts, v + 1, PANEL_FAULT_CODES);
      for (i = 0u, v += 2u + PANEL_FAULT_CODES; i < count; ++i, v += PANEL_ALERTX_STRIDE) {
        s->alerts[i].severity = v[0]; s->alerts[i].code = v[1]; s->alerts[i].ageMin = readLe16(v + 2);
        readName(s->alerts[i].host, v + 4, PANEL_NAME_SYS);
        readName(s->alerts[i].msg, v + 16, PANEL_ALERT_MSG);
      }
      s->alertCount = (uint8_t)count; s->hasAlerts = 1u;
      return;
    case PANEL_TLV_PROBES:
      if (n < 1u || (count = v[0]) > PANEL_MAX_PROBES || n < 1u + count * PANEL_PROBE_STRIDE) return;
      for (i = 0u, v += 1; i < count; ++i, v += PANEL_PROBE_STRIDE) {
        readName(s->probes[i].name, v, PANEL_NAME_PROBE);
        s->probes[i].rttTenthMs = readLe16(v + 8); s->probes[i].lossPermille = readLe16(v + 10);
        s->probes[i].state = v[12];
      }
      s->probeCount = (uint8_t)count; s->hasProbes = 1u;
      return;
    case PANEL_TLV_POP:
      if (n < 1u || (count = v[0]) > PANEL_MAX_POP || n < 1u + count) return;
      memcpy(s->pop, v + 1, count); s->popCount = (uint8_t)count;
      return;
    case PANEL_TLV_HEAT:
      if (n < 2u * PANEL_HEAT_LEN) return;
      memcpy(s->heatWarn, v, PANEL_HEAT_LEN); memcpy(s->heatCrit, v + PANEL_HEAT_LEN, PANEL_HEAT_LEN);
      s->hasHeat = 1u;
      return;
    case PANEL_TLV_TOTALS:
      if (n < 16u) return;
      s->wanUpKbps = readLe32(v); s->wanDownKbps = readLe32(v + 4);
      s->lanUpKbps = readLe32(v + 8); s->lanDownKbps = readLe32(v + 12);
      s->hasTotals = 1u;
      return;
    default:
      return; /* unknown tag: a newer sender, not an error */
  }
}

/* Purpose: walk the TLV trailer. Input: bytes after the gear trailer/length/snapshot. Output: none; a truncated TLV ends the walk without failing the snapshot. */
static void panelExtRead(const uint8_t *p, size_t len, PanelSnapshot *s) {
  while (len >= 3u) {
    size_t n = readLe16(p + 1);
    if (len - 3u < n) return;
    panelExtValue(p[0], p + 3, n, s);
    p += 3u + n; len -= 3u + n;
  }
}

/* Purpose: encode canonical snapshot payload. Input: snapshot/output buffer. Output: payload size, or zero on invalid capacity/counts. */
size_t panelEncodeSnapshot(const PanelSnapshot *s, uint8_t *p, size_t cap) {
  size_t i, off, need, ext;
  if (s == NULL || p == NULL || s->poolCount > PANEL_MAX_POOLS || s->systemCount > PANEL_MAX_SYSTEMS || s->gearCount > PANEL_MAX_GEAR) return 0u;
  need = PANEL_GLOBALS_SIZE + PANEL_POOL_STRIDE * (size_t)s->poolCount + PANEL_SYS_STRIDE * (size_t)s->systemCount + (s->hasTopAlert ? PANEL_ALERT_SIZE : 0u) + 1u + 4u * (size_t)s->gearCount;
  if (s->linkCount > PANEL_MAX_LINKS || s->alertCount > PANEL_MAX_ALERTS || s->probeCount > PANEL_MAX_PROBES || s->popCount > PANEL_MAX_POP) return 0u;
  ext = panelExtSize(s);
  if (cap < need + ext || need + ext > PANEL_MAX_PAYLOAD) return 0u;
  memset(p, 0, need);
  writeLe32(p, s->ts); writeLe16(p + 4, s->seq); writeLe16(p + 6, s->score); p[8] = s->alarmActive; p[9] = s->dataStale; memcpy(p + 10, s->stateRoll, 5u); memcpy(p + 15, s->alertCounts, 3u); p[18] = s->meanLoadPct; p[19] = s->poolCount; p[20] = s->systemCount; p[21] = s->hasTopAlert ? 1u : 0u; writeLe32(p + 22, s->rxKbps); writeLe32(p + 26, s->txKbps); writeLe16(p + 30, s->rttTenthMs); writeLe16(p + 32, s->lossPermille);
  off = PANEL_GLOBALS_SIZE;
  for (i = 0; i < s->poolCount; ++i, off += PANEL_POOL_STRIDE) { writeName(p + off, s->pools[i].name, PANEL_NAME_POOL); p[off+8] = s->pools[i].tier; p[off+9] = s->pools[i].ok; p[off+10] = s->pools[i].degraded; p[off+11] = s->pools[i].down; p[off+12] = s->pools[i].unknown; p[off+13] = s->pools[i].maint; p[off+14] = s->pools[i].total; p[off+15] = s->pools[i].loadPct; }
  for (i = 0; i < s->systemCount; ++i, off += PANEL_SYS_STRIDE) { writeName(p + off, s->systems[i].name, PANEL_NAME_SYS); p[off+12] = s->systems[i].state; p[off+13] = s->systems[i].pool; p[off+14] = s->systems[i].tier; p[off+15] = s->systems[i].loadPct; }
  if (s->hasTopAlert) { writeLe32(p + off, s->topAlert.alertId); writeLe32(p + off + 4, s->topAlert.episodeId); p[off+8] = s->topAlert.severity; writeName(p + off + 12, s->topAlert.subject, PANEL_ALERT_SUBJ); writeName(p + off + 36, s->topAlert.detail, PANEL_ALERT_DETAIL); off += PANEL_ALERT_SIZE; }
  p[off++] = s->gearCount;
  for (i = 0u; i < s->gearCount; ++i, off += 4u) { p[off] = s->gear[i].role; p[off + 1u] = s->gear[i].state; p[off + 2u] = s->gear[i].rxLevel; p[off + 3u] = s->gear[i].txLevel; }
  panelExtWrite(s, p + off);
  return need + ext;
}

/* Purpose: decode canonical snapshot payload. Input: complete payload. Output: filled snapshot or -1 for malformed layout. */
int panelDecodeSnapshot(const uint8_t *p, size_t len, PanelSnapshot *s) {
  size_t i, off, need; uint8_t pools, systems, alert, gears;
  if (p == NULL || s == NULL || len < PANEL_GLOBALS_SIZE) return -1;
  pools = p[19]; systems = p[20]; alert = p[21]; if (pools > PANEL_MAX_POOLS || systems > PANEL_MAX_SYSTEMS || alert > 1u) return -1;
  need = PANEL_GLOBALS_SIZE + PANEL_POOL_STRIDE * (size_t)pools + PANEL_SYS_STRIDE * (size_t)systems + (alert ? PANEL_ALERT_SIZE : 0u); if (len < need) return -1;
  memset(s, 0, sizeof(*s)); s->ts = readLe32(p); s->seq = readLe16(p+4); s->score = readLe16(p+6); s->alarmActive=p[8]; s->dataStale=p[9]; memcpy(s->stateRoll,p+10,5u); memcpy(s->alertCounts,p+15,3u); s->meanLoadPct=p[18]; s->poolCount=pools; s->systemCount=systems; s->hasTopAlert=alert; s->rxKbps=readLe32(p+22); s->txKbps=readLe32(p+26); s->rttTenthMs=readLe16(p+30); s->lossPermille=readLe16(p+32); off=PANEL_GLOBALS_SIZE;
  for(i=0;i<pools;++i,off+=PANEL_POOL_STRIDE){ readName(s->pools[i].name,p+off,PANEL_NAME_POOL); s->pools[i].tier=p[off+8]; s->pools[i].ok=p[off+9]; s->pools[i].degraded=p[off+10]; s->pools[i].down=p[off+11]; s->pools[i].unknown=p[off+12]; s->pools[i].maint=p[off+13]; s->pools[i].total=p[off+14]; s->pools[i].loadPct=p[off+15]; }
  for(i=0;i<systems;++i,off+=PANEL_SYS_STRIDE){ readName(s->systems[i].name,p+off,PANEL_NAME_SYS); s->systems[i].state=p[off+12]; s->systems[i].pool=p[off+13]; s->systems[i].tier=p[off+14]; s->systems[i].loadPct=p[off+15]; }
  if(alert){ s->topAlert.alertId=readLe32(p+off); s->topAlert.episodeId=readLe32(p+off+4); s->topAlert.severity=p[off+8]; readName(s->topAlert.subject,p+off+12,PANEL_ALERT_SUBJ); readName(s->topAlert.detail,p+off+36,PANEL_ALERT_DETAIL); off += PANEL_ALERT_SIZE; }
  /* CONTRACT-AW §10.1: the v1 gear trailer is same-version and additive.
   * It is present only when its count and all entries fit; absent, short, or
   * oversized trailers are valid no-data, never a base SNAPSHOT error. */
  if (len == off) return 0;
  gears = p[off++];
  if (gears > PANEL_MAX_GEAR || len < off + 4u * (size_t)gears) return 0;
  s->gearCount = gears;
  for (i = 0u; i < gears; ++i, off += 4u) { s->gear[i].role = p[off]; s->gear[i].state = p[off + 1u]; s->gear[i].rxLevel = p[off + 2u]; s->gear[i].txLevel = p[off + 3u]; }
  panelExtRead(p + off, len - off, s);
  return 0;
}

/* Purpose: keep a possible overlapping magic suffix after parser rejection. Input: parser/start offset. Output: parser buffer repositioned. */
static void parserResync(PanelParser *p, size_t start) { size_t i, remain = 0u; for (i=start;i+1u<p->have;++i) if(p->buf[i]==PANEL_MAGIC0 && p->buf[i+1u]==PANEL_MAGIC1){ remain=p->have-i; memmove(p->buf,p->buf+i,remain); break; } if(remain==0u && p->have>start && p->buf[p->have-1u]==PANEL_MAGIC0){p->buf[0]=PANEL_MAGIC0;remain=1u;} p->have=remain; p->resyncs++; }
/* Purpose: identify defined frame types. Input: type byte. Output: nonzero when dispatchable. */
static int knownType(uint8_t type) { return type >= PANEL_FT_SNAPSHOT && type <= PANEL_FT_PROVISION ? 1 : (type >= PANEL_FT_HELLO && type <= PANEL_FT_PROVACK ? 1 : 0); }
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
      /* CONTRACT-AW §10.1: gear is a presence-detected, same-version v1
       * trailing extension. Framing itself remains strict-version. */
      if(p->buf[2] == PANEL_PROTO_VERSION && knownType(p->buf[3]) && cb!=NULL) cb(p->buf[3],p->buf+PANEL_HDR_SIZE,plen,user);
      remain=p->have-total;
      if(remain!=0u) memmove(p->buf,p->buf+total,remain);
      p->have=remain;
    }
  }
}
