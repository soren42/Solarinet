/*
 * protocol.h — SolariNet status-panel wire protocol (NORMATIVE).
 *
 * Single source of truth for the USB-CDC link between the solariPanel daemon
 * (lithium, host) and the Galactic Unicorn firmware (RP2350). Both sides
 * compile the SAME shared codec (protocol.c) against this header; neither
 * side may cast wire bytes to native structs — all access goes through the
 * encode/decode functions below, which read/write fixed offsets. This is
 * deliberate: compiler padding, enum width and endianness never touch the
 * wire. Authored by the Lead per CONTRACT.md §4/v1.1; amendments go through
 * the Lead, not peer agreement.
 *
 * Wire format (all multi-byte fields little-endian):
 *
 *   offset  size  field
 *   0       1     magic0 = 0xA5
 *   1       1     magic1 = 0x53
 *   2       1     version = PANEL_PROTO_VERSION
 *   3       1     type (PanelFrameType)
 *   4       2     payloadLen (LE, <= PANEL_MAX_PAYLOAD)
 *   6       n     payload
 *   6+n     2     crc16-CCITT (poly 0x1021, init 0xFFFF) over bytes 2..6+n-1
 *                 (version, type, payloadLen, payload — NOT the magic)
 *
 * Parser rules (both sides): scan byte-by-byte for magic0,magic1 (overlapping
 * matches allowed); reject payloadLen > PANEL_MAX_PAYLOAD before buffering;
 * abandon a partial frame after PANEL_FRAME_TIMEOUT_MS without new bytes and
 * resume scanning at the byte after the failed magic; on CRC mismatch drop
 * the frame, bump a counter, resume scanning inside the dropped bytes.
 * Unknown type or version: skip the frame (length is still trusted if it
 * passed the CRC), never desync. Receivers apply only complete, CRC-valid
 * snapshots, and ignore a snapshot whose seq equals the last applied seq
 * (duplicate); older seq (wraparound-aware, RFC1982 style) is also ignored.
 */

#ifndef SOLARI_PANEL_PROTOCOL_H
#define SOLARI_PANEL_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- framing ------------------------------------------------------------ */

#define PANEL_MAGIC0            0xA5u
#define PANEL_MAGIC1            0x53u
#define PANEL_PROTO_VERSION     0x01u
#define PANEL_HDR_SIZE          6u      /* magic..payloadLen               */
#define PANEL_CRC_SIZE          2u
#define PANEL_MAX_PAYLOAD       2048u
#define PANEL_FRAME_TIMEOUT_MS  500u

typedef enum {                     /* host -> panel */
  PANEL_FT_SNAPSHOT = 0x01,        /* full panel state, §snapshot below     */
  PANEL_FT_PING     = 0x02,        /* empty payload, link-alive             */
  PANEL_FT_HELLOREQ = 0x03,        /* empty payload, ask panel for HELLO    */
                                   /* panel -> host (diagnostics only)      */
  PANEL_FT_HELLO    = 0x81,        /* u8 protoVer, u8 fwMajor, u8 fwMinor,
                                      then ASCII build string (<=32)        */
  PANEL_FT_EVENT    = 0x82,        /* u8 kind (PanelEventKind), u8 arg      */
  PANEL_FT_LOG      = 0x83         /* ASCII text (<=128)                    */
} PanelFrameType;

typedef enum {
  PANEL_EV_BUTTON      = 0x01,     /* arg = button index 0..8               */
  PANEL_EV_ACK         = 0x02,     /* arg = 0; alarm acknowledged           */
  PANEL_EV_THEMECHANGE = 0x03,     /* arg = theme 0..3                      */
  PANEL_EV_LINKLOST    = 0x04,     /* arg = 0; snapshot staleness tripped   */
  PANEL_EV_LINKBACK    = 0x05      /* arg = 0; snapshots flowing again      */
} PanelEventKind;

/* ---- canonical state / severity enums (wire values, u8) ----------------- */

typedef enum {
  PANEL_ST_OK       = 0,
  PANEL_ST_DEGRADED = 1,
  PANEL_ST_DOWN     = 2,
  PANEL_ST_UNKNOWN  = 3,
  PANEL_ST_MAINT    = 4
} PanelState;

typedef enum {
  PANEL_SEV_INFO = 0,
  PANEL_SEV_WARN = 1,
  PANEL_SEV_CRIT = 2
} PanelSeverity;

/* ---- SNAPSHOT payload layout (fixed offsets, LE) ------------------------ *
 *
 * globals   offset 0, 40 bytes:
 *   0   u32 ts             unix seconds, server clock at composition
 *   4   u16 seq            monotonic, wraps; duplicates ignored by receiver
 *   6   u16 score          design-brief poolScore output (worst pool; the
 *                          current formula emits 0..~140 — receivers must
 *                          treat the range as formula-defined, not capped)
 *   8   u8  alarmActive    inlay driver: crit alert OR tier-threshold breach
 *   9   u8  dataStale      1 if API data older than 30 s at composition
 *   10  u8[5] stateRoll    fleet counts: ok,degraded,down,unknown,maint
 *                          (maint is reserved: node.state has no maint
 *                          member today, so it is structurally 0)
 *   15  u8[3] alertCounts  info,warn,crit — dashboard-active rule, clamp 255
 *   18  u8  meanLoadPct    0..100
 *   19  u8  poolCount      0..PANEL_MAX_POOLS
 *   20  u8  systemCount    0..PANEL_MAX_SYSTEMS
 *   21  u8  hasTopAlert    0/1
 *   22  u32 rxKbps         fleet aggregate ingress
 *   26  u32 txKbps         fleet aggregate egress
 *   30  u16 rttTenthMs     mean probe RTT, 0.1 ms units (0 = no probes)
 *   32  u16 lossPermille   mean probe loss (0 = none/no probes)
 *   34  u8[6] reserved     zero on send, ignored on receive
 *
 * pools     offset 40, stride 20, poolCount entries:
 *   +0  char[8] name       uppercase ASCII [A-Z0-9 .:-], NUL-padded
 *   +8  u8 tier            0..3
 *   +9  u8 ok  +10 u8 degraded  +11 u8 down  +12 u8 unknown  +13 u8 maint
 *   +14 u8 total           membership count (>= sum of states)
 *   +15 u8 loadPct         0..100
 *   +16 u8[4] reserved
 *
 * systems   offset 40 + 20*poolCount, stride 16, systemCount entries:
 *   +0  char[12] name      same charset, NUL-padded, truncated w/ final '.'
 *   +12 u8 state           PanelState
 *   +13 u8 pool            index into pools[], 0xFF = unpooled
 *   +14 u8 tier            0..3
 *   +15 u8 loadPct         0..100 (0 with state UNKNOWN = no telemetry)
 *
 * topAlert  (only if hasTopAlert) after systems, 84 bytes:
 *   +0  u32 alertId        alertEvent.eventId of the shown alert
 *   +4  u32 episodeId      monotonic alarm episode; CHANGE re-arms the tone.
 *                          Composed server-side; a new triggering alert set
 *                          gets a new episodeId, ack'd episodes never reuse.
 *   +8  u8 severity        PanelSeverity
 *   +9  u8[3] reserved
 *   +12 char[24] subject   uppercase, NUL-padded
 *   +36 char[48] detail    uppercase remediation hint, NUL-padded
 *
 * History (load ribbons, histograms, RTT sparklines) is NOT on the wire:
 * the firmware accumulates it from successive snapshots (2 s cadence).
 * ------------------------------------------------------------------------- */

#define PANEL_MAX_POOLS    8u
#define PANEL_MAX_SYSTEMS  64u
#define PANEL_GLOBALS_SIZE 40u
#define PANEL_POOL_STRIDE  20u
#define PANEL_SYS_STRIDE   16u
#define PANEL_ALERT_SIZE   84u
#define PANEL_NAME_POOL    8u
#define PANEL_NAME_SYS     12u
#define PANEL_ALERT_SUBJ   24u
#define PANEL_ALERT_DETAIL 48u

/* Decoded snapshot — the IN-MEMORY struct both sides use after decode.
 * Never written to the wire directly.                                       */
typedef struct {
  uint32_t ts;
  uint16_t seq;
  uint16_t score;
  uint8_t  alarmActive, dataStale;
  uint8_t  stateRoll[5];
  uint8_t  alertCounts[3];
  uint8_t  meanLoadPct;
  uint8_t  poolCount, systemCount, hasTopAlert;
  uint32_t rxKbps, txKbps;
  uint16_t rttTenthMs, lossPermille;
  struct {
    char    name[PANEL_NAME_POOL + 1];
    uint8_t tier, ok, degraded, down, unknown, maint, total, loadPct;
  } pools[PANEL_MAX_POOLS];
  struct {
    char    name[PANEL_NAME_SYS + 1];
    uint8_t state, pool, tier, loadPct;
  } systems[PANEL_MAX_SYSTEMS];
  struct {
    uint32_t alertId, episodeId;
    uint8_t  severity;
    char     subject[PANEL_ALERT_SUBJ + 1];
    char     detail[PANEL_ALERT_DETAIL + 1];
  } topAlert;
} PanelSnapshot;

/* ---- shared codec (implemented once, in protocol.c) --------------------- */

/* crc16-CCITT, poly 0x1021, init 0xFFFF — over version..payload. */
uint16_t panelCrc16(const uint8_t *data, size_t len);

/* panelSeqNewer — RFC1982 serial-number comparison for snapshot seq.
 * Input:  candidate and lastApplied sequence numbers.
 * Output: 1 if candidate is newer than lastApplied (wraparound-aware),
 *         0 if equal or older. BOTH sides use this — never hand-roll.     */
int panelSeqNewer(uint16_t candidate, uint16_t lastApplied);

/* panelEncodeFrame — write a complete frame into out.
 * Input:  type, payload/payloadLen (payload may be NULL when len 0),
 *         out buffer of at least PANEL_HDR_SIZE+payloadLen+PANEL_CRC_SIZE.
 * Output: total frame length, or 0 if payloadLen > PANEL_MAX_PAYLOAD.      */
size_t panelEncodeFrame(uint8_t type, const uint8_t *payload,
                        size_t payloadLen, uint8_t *out, size_t outCap);

/* panelEncodeSnapshot — serialize snap into payload buffer (layout above).
 * Returns payload length, or 0 if counts exceed the protocol maxima.
 * Names are sanitized here: uppercased, charset-filtered, NUL-padded.      */
size_t panelEncodeSnapshot(const PanelSnapshot *snap,
                           uint8_t *payload, size_t cap);

/* panelDecodeSnapshot — parse a SNAPSHOT payload into snap.
 * Returns 0 on success, -1 on malformed input (bad counts/length).
 * Trailing bytes beyond the computed layout are ignored (fwd compat).      */
int panelDecodeSnapshot(const uint8_t *payload, size_t len,
                        PanelSnapshot *snap);

/* Incremental frame parser: feed bytes as they arrive, cb fires per valid
 * frame. Internal state machine implements the parser rules above.         */
typedef void (*PanelFrameCb)(uint8_t type, const uint8_t *payload,
                             size_t len, void *user);
typedef struct {
  uint8_t  buf[PANEL_HDR_SIZE + PANEL_MAX_PAYLOAD + PANEL_CRC_SIZE];
  size_t   have;                /* bytes accumulated for the current frame  */
  uint32_t lastByteMs;          /* for PANEL_FRAME_TIMEOUT_MS abandonment   */
  uint32_t crcErrors, resyncs;  /* diagnostics                              */
} PanelParser;

void panelParserInit(PanelParser *p);
/* nowMs: caller-supplied monotonic milliseconds (for the frame timeout).   */
void panelParserFeed(PanelParser *p, const uint8_t *bytes, size_t n,
                     uint32_t nowMs, PanelFrameCb cb, void *user);

#ifdef __cplusplus
}
#endif
#endif /* SOLARI_PANEL_PROTOCOL_H */
