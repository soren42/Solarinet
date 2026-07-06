/*
 * server.h - solariServer internal API and shared contract (§9, §10, §11;
 * Phase 3 Handoff §4-§7).
 *
 * The server is the authoritative tier: it ingests CLIENT/MONITOR/WATCHDOG
 * reports and DISCOVERY/TOPOLOGY adverts, persists them to MariaDB, owns the
 * failover lease and master election among server instances, evaluates alert
 * rules, drives the control plane (config/deploy/decommission), and exposes the
 * operator bridge (solariCtl) the PHP dashboard drives over a Unix socket.
 *
 * Mirrors the monitor's build shape: every subsystem .c compiles into
 * libservercore.a; src/server/main.c links it into the thin solariServer
 * binary. This header is the single shared contract - subsystem implementers
 * own exactly one .c and MUST NOT edit this header.
 *
 * Conventions (libsolari): every function returns solariStatus (SOLARI_OK or a
 * negative solariError); out-params are written only on success unless noted;
 * handle state lives in caller-threaded structs, no globals.
 */
#ifndef SOLARI_SERVER_H
#define SOLARI_SERVER_H

#include "solari/solariCommon.h"
#include "solari/solariFrame.h"
#include "solari/solariMsg.h"
#include "solari/solariNet.h"

/* ---- fixed bounds ---- */
#define SERVER_URL_MAX        256
#define SERVER_PATH_MAX       512
#define SERVER_CN_MAX         256
#define SERVER_TARGETID_MAX   128
#define SERVER_IP_MAX         64
#define SERVER_SEGID_MAX      32
#define SERVER_GEARID_MAX     48
#define SERVER_VERSION_MAX    32
#define SERVER_SHA256_HEX_LEN 64
#define SERVER_DEDUP_WINDOW   1024  /* per-source recent-seqNo ring size */

/* ===================================================================== */
/* Forward declarations                                                   */
/* ===================================================================== */
typedef struct serverDb      serverDb;       /* opaque MariaDB handle wrapper */
typedef struct serverCtl     serverCtl;      /* opaque solariCtl bridge       */
typedef struct serverContext serverContext;  /* the server's whole world      */

/* ===================================================================== */
/* Configuration (loaded from the server .conf; §13)                      */
/* ===================================================================== */
typedef struct {
    /* identity / transport */
    char     hostFqdn[SOLARI_FQDN_MAX];      /* override; empty = autodetect */
    uint64_t nodeId;                         /* 0 = derive from fqdn+role    */
    char     ingestUrl[SERVER_URL_MAX];      /* PULL listener for reports    */
    char     surveyUrl[SERVER_URL_MAX];      /* SURVEYOR bind (immediate rnd)*/
    char     controlUrl[SERVER_URL_MAX];     /* REP/PUB for control + lease  */
    char     pubUrl[SERVER_URL_MAX];         /* PUB bind (alerts -> ntfy)    */
    bool     useTls;
    char     caFile[SERVER_PATH_MAX];        /* internal CA root (verify peers) */
    char     certFile[SERVER_PATH_MAX];      /* this server's TLS cert          */
    char     keyFile[SERVER_PATH_MAX];       /* this server's TLS key           */

    /* internal CA (signs node certs). Deliberately separate from the server's
     * own TLS material so the signing CA can be relocated to a dedicated host:
     * caMode "local" signs here with caKeyFile; "remote" delegates to caUrl (a
     * future CA service). caCertFile defaults to caFile when unset. (§15.1) */
    char     caCertFile[SERVER_PATH_MAX];    /* CA cert that issues leaf certs  */
    char     caKeyFile[SERVER_PATH_MAX];     /* CA private key (signing); never on web tier */
    char     caMode[8];                      /* "local" | "remote" (default local) */
    char     caUrl[SERVER_URL_MAX];          /* remote CA signer endpoint (caMode=remote) */

    /* database (MariaDB / Connector-C) */
    char     dbHost[SERVER_URL_MAX];
    uint16_t dbPort;                         /* default 3306                 */
    char     dbName[64];
    char     dbUser[64];
    char     dbPass[128];
    char     dbSocket[SERVER_PATH_MAX];      /* empty = TCP                  */
    uint8_t  dbPoolSize;                     /* default 4                    */

    /* failover / lease (§9.2) */
    uint32_t leaseRenewSec;                  /* default 5                    */
    uint32_t leaseTtlSec;                    /* default 15                   */

    /* operator bridge (solariCtl) */
    char     ctlSocket[SERVER_PATH_MAX];     /* Unix domain socket path      */

    /* discovery / provisioning policy (Handoff §7.1) - conservative default */
    bool     autoDiscover;                   /* default true                 */
    bool     autoEnroll;                     /* default false                */
} serverConfig;

/* Defaults: dbPort 3306, pool 4, lease 5/15s, autoDiscover on, autoEnroll off. */
void serverConfigDefaults(serverConfig *out);
/* Load a server .conf (§13). Absent keys take defaults. */
solariStatus serverConfigFromFile(const char *path, serverConfig *out);
/* Stable node id: configured id, or FNV-1a-64(fqdn|ROLE_SERVER) (§5.5). */
uint64_t serverNodeId(const serverConfig *cfg);

/* ===================================================================== */
/* Failover role state (§9.2)                                             */
/* ===================================================================== */
typedef enum { SRV_STANDBY = 0, SRV_ACTIVE = 1 } serverRoleState;

/* ===================================================================== */
/* serverContext - the world threaded through every subsystem            */
/* ===================================================================== */
struct serverContext {
    const serverConfig *cfg;
    serverDb           *db;          /* persistence handle                  */
    serverCtl          *ctl;         /* operator bridge (may be NULL)       */

    uint64_t            nodeId;      /* this server's id                    */
    serverRoleState     role;        /* current lease role                  */
    uint64_t            epoch;       /* lease epoch (advances on fresh claim)*/

    /* bound transport endpoints (owned here; NULL until serverApplyRole) */
    solariConn         *ingest;      /* PULL sink                           */
    solariConn         *control;     /* REP for who-is-primary / control    */
    solariConn         *pub;         /* PUB for alerts                      */
};

/* Wire cfg into ctx, derive nodeId, zero handles. Does not open db/sockets. */
solariStatus serverContextInit(serverContext *ctx, const serverConfig *cfg);
/* Tear down: unbind sockets, close ctl. Does NOT close db (caller owns it). */
void         serverContextClose(serverContext *ctx);

/* ===================================================================== */
/* serverDb.c - persistence (MOST depended-upon; §9.1, §10)              */
/* ===================================================================== */

/* Open a MariaDB connection/pool from cfg. *out gets the handle. Returns
 * ERR_DB on connect failure, ERR_INVALID_ARG on bad opts. */
solariStatus serverDbOpen(const serverConfig *cfg, serverDb **out);
/* Close all connections and free the handle (NULL-safe). */
void         serverDbClose(serverDb *db);
/* Liveness ping; ERR_DB if the connection is dead (caller may reconnect). */
solariStatus serverDbPing(serverDb *db);

/* Apply a .sql script (schema.sql or a migration) as a single batch. Used by
 * --init-db. Returns ERR_DB on any statement failure. */
solariStatus serverDbApplyScript(serverDb *db, const char *sqlPath);

/* ---- node identity / lifecycle (§10 node) ---- */
/* Upsert the node row from a HELLO (role, fqdn, cert CN, os/arch, epoch).
 * Sets enrolledAt on first insert; always bumps lastSeenAt. */
solariStatus serverDbUpsertNode(serverDb *db, uint64_t nodeId, solariRole role,
                                const char *hostFqdn, const char *certCn,
                                const char *osName, const char *arch,
                                uint64_t configEpoch);
/* Touch lastSeenAt for a node (cheap heartbeat write). */
solariStatus serverDbTouchNode(serverDb *db, uint64_t nodeId, uint64_t whenUnixMs);
/* Set derived health state ('up'/'degraded'/'down'/'unknown'/'retired'). */
solariStatus serverDbSetNodeState(serverDb *db, uint64_t nodeId, const char *state);

/* Upsert a probe target (the authoritative catalog /api/probes reads). Created
 * when a discovered entity is adopted so the target is visible (and assignable
 * to monitors) immediately, before any vantage has reported. proto is
 * 'icmp'|'tcp'|'udp'; port 0 for icmp. assetId links it to its owning system
 * (0 = unlinked). Keyed by targetId. */
solariStatus serverDbUpsertProbeTarget(serverDb *db, const char *targetId,
                                       const char *host, int port,
                                       const char *proto, int replFactor,
                                       const char *label, const char *segId,
                                       uint64_t assetId,
                                       const char *probeType,
                                       const char *checkArg);
/* Delete a probe target by id (e.g. when a host heartbeat is turned off). */
solariStatus serverDbDeleteProbeTarget(serverDb *db, const char *targetId);
/* Delete probeCurrent + probeHistory rows for a target before removing catalog. */
solariStatus serverDbPurgeProbeState(serverDb *db, const char *targetId);

/* ---- assets / pools (monitored systems + functional groups; §10 ext) ---- */
/* Upsert a monitored system keyed by ip; *assetId returns the row id. poolId 0
 * leaves it unassigned; tagsJson may be NULL. monitorHost toggles the heartbeat
 * intent (the ICMP target itself is (de)provisioned by the caller). */
solariStatus serverDbUpsertAsset(serverDb *db, const char *ip, const char *host,
                                 const char *displayName, const char *className,
                                 uint64_t poolId, const char *tagsJson,
                                 const char *notes, bool monitorHost,
                                 uint64_t *assetId);
/* Look up an asset id by ip (*assetId = 0 if none). */
solariStatus serverDbGetAssetIdByIp(serverDb *db, const char *ip, uint64_t *assetId);
/* List targetIds owned by an asset; out is [cap][SERVER_TARGETID_MAX]. */
solariStatus serverDbListAssetTargets(serverDb *db, uint64_t assetId,
                                      char out[][SERVER_TARGETID_MAX],
                                      size_t cap, size_t *count);
/* Delete an asset row. */
solariStatus serverDbDeleteAsset(serverDb *db, uint64_t assetId);
/* Create a functional pool; *poolId returns the id. ERR_DB on a duplicate name. */
solariStatus serverDbCreatePool(serverDb *db, const char *name, const char *desc,
                                const char *color, uint64_t *poolId);
/* Update a pool's fields (NULL leaves a field unchanged). */
solariStatus serverDbUpdatePool(serverDb *db, uint64_t poolId, const char *name,
                                const char *desc, const char *color);
solariStatus serverDbReassignPoolAssets(serverDb *db, uint64_t fromPool,
                                        uint64_t toPool, size_t *n);
solariStatus serverDbDeletePool(serverDb *db, uint64_t poolId);

/* ---- global config / rules (operator config writes; §10) ---- */
/* Persist the fleet-wide config document and bump its epoch; *epoch returns the
 * new epoch. configJson is stored verbatim. */
solariStatus serverDbSetGlobalConfig(serverDb *db, const char *configJson,
                                     const char *updatedBy, uint64_t *epoch);
/* Update one alertRule. Each field is applied only if its hasXxx flag is set. */
typedef struct {
    int    ruleId;
    bool   hasEnabled;    bool   enabled;
    bool   hasThreshold;  double threshold;
    bool   hasForSeconds; int    forSeconds;
    bool   hasOp;         char   op[12];
    bool   hasSeverity;   char   severity[8];
    bool   hasMetric;     char   metric[64];
    bool   hasScope;      char   scope[8];
} serverAlertRuleEdit;
solariStatus serverDbUpdateAlertRule(serverDb *db, const serverAlertRuleEdit *e);
solariStatus serverDbDeleteAlertRule(serverDb *db, uint64_t ruleId);
solariStatus serverDbAckAlertEvent(serverDb *db, uint64_t eventId);

/* ---- report persistence (§9.1 representative writers, §10) ---- */
/* Upsert hostCurrent + append hostHistory in one txn (§9.1). All time UTC. */
solariStatus serverDbWriteClientReport(serverContext *ctx, uint64_t nodeId,
                                       const solariClientReport *rep);
/* Upsert probeCurrent + append probeHistory per probe result, one txn. */
solariStatus serverDbWriteMonitorReport(serverContext *ctx, uint64_t nodeId,
                                        const solariMonitorReport *rep);

/* ---- alerts / audit (§10 alertEvent) ---- */
/* Append an alertEvent row (alerting + the decommission audit trail). targetId
 * may be NULL for host-scoped events; ruleId 0 for synthetic/audit rows. */
solariStatus serverDbWriteAlertEvent(serverDb *db, int ruleId, uint64_t nodeId,
                                     const char *targetId, const char *severity,
                                     const char *detail, uint64_t firedUnixMs);
/* Load all enabled alert rules of the given scope ('host' or 'probe'). Writes
 * up to *count rules into out (caller-sized); updates *count to the number
 * written. */
typedef struct {
    int      ruleId;
    char     scope[8];     /* "host" | "probe"               */
    char     metric[64];
    char     op[12];       /* "gt" | "lt" | "eq" | "transition" */
    double   threshold;
    int      forSeconds;
    char     severity[8];  /* "info" | "warn" | "crit"       */
} serverAlertRule;
solariStatus serverDbLoadAlertRules(serverDb *db, const char *scope,
                                    serverAlertRule *out, size_t *count);

/* ---- config / convergence (§10 nodeConfig) ---- */
/* Set the per-node target epoch + config blob (drives a CTRL_SET_CONFIG). */
solariStatus serverDbSetNodeTargetEpoch(serverDb *db, uint64_t nodeId,
                                        uint64_t targetEpoch, const char *configJson);
/* Record a control-directive outcome: appliedEpoch + lastResult + when. */
solariStatus serverDbSetNodeApplied(serverDb *db, uint64_t nodeId,
                                    uint64_t appliedEpoch, const char *lastResult,
                                    uint64_t whenUnixMs);

/* ---- lease ops (§10 serverLease) ---- */
/* The conditional UPDATE that IS the election: claim/renew the single lease row
 * for `selfNodeId` with a fresh expiry. *won=true if this call holds the lease
 * afterward; *epoch returns the current lease epoch (advanced on a fresh claim
 * from a different/expired holder). Single txn. */
solariStatus serverDbLeaseClaim(serverDb *db, uint64_t selfNodeId,
                                uint32_t ttlSec, bool *won, uint64_t *epoch);
/* Read the current lease holder + epoch (for WHO_IS_PRIMARY answers). *holder
 * is 0 if the lease is unheld/expired. */
solariStatus serverDbLeaseRead(serverDb *db, uint64_t *holder, uint64_t *epoch);

/* ---- discovery (Handoff §5 discovered, §7.1) ---- */
typedef struct {
    char     host[SOLARI_FQDN_MAX];
    char     ip[SERVER_IP_MAX];          /* required, part of unique key    */
    char     kind[12];                   /* "host"|"service"|"network"|"other" */
    char     via[12];                    /* "mdns"|"arp"|"scp_advert"|"portscan"|"lldp" */
    char     servicesJson[1024];         /* ["ssh:22","http:80"]; may be "" */
    char     segId[SERVER_SEGID_MAX];
    char     arch[SOLARI_ARCH_MAX];
    char     mac[18];
    char     vendor[64];
    char     osName[64];
    char     deviceRole[24];
    char     sysDescr[256];
    char     mdnsName[128];
} serverDiscEntity;
/* Upsert a discovered candidate keyed (ip, kind): insert new or bump seenCount
 * + lastSeenAt. *discId returns the row id. */
solariStatus serverDbUpsertDiscovered(serverDb *db, const serverDiscEntity *e,
                                      uint64_t whenUnixMs, uint64_t *discId);
/* Set a discovered row's status ('new'|'ignored'|'adopting'|'adopted'). */
solariStatus serverDbSetDiscoveredStatus(serverDb *db, uint64_t discId,
                                         const char *status);
/* True (via *exists) if (ip,kind) already corresponds to an enrolled node -
 * used to keep already-monitored entities out of the "new" list. */
solariStatus serverDbDiscoveredIsKnown(serverDb *db, const char *ip,
                                       const char *kind, bool *exists);
/* Load a single discovered candidate by id (for adoption resolution). */
solariStatus serverDbGetDiscovered(serverDb *db, uint64_t discId,
                                   serverDiscEntity *out);

/* ---- topology (Handoff §5 networkGear, lldpEdge; §7.2) ---- */
typedef struct {
    uint64_t nodeId;                     /* endpoint node, 0 if not a node  */
    char     gearId[SERVER_GEARID_MAX];  /* switch/AP side, "" if unknown   */
    char     localIf[48];
    char     peerPort[48];
    char     linkType[12];               /* "wired" | "wireless"            */
    int      speedMbps;
    int      rssi;
    bool     viaLldp;
} serverLldpEdge;
/* Append/refresh an lldpEdge observation. */
solariStatus serverDbWriteLldpEdge(serverDb *db, const serverLldpEdge *edge,
                                   uint64_t whenUnixMs);
/* Upsert a networkGear inventory row (gateway/switch/ap/router/other). */
typedef struct {
    char     gearId[SERVER_GEARID_MAX];
    char     name[96];
    char     kind[12];                   /* "gateway"|"switch"|"ap"|"router"|"other" */
    char     model[96];
    char     segId[SERVER_SEGID_MAX];
    int      ports;
    char     uplinkGearId[SERVER_GEARID_MAX];
    bool     wireless;
    char     mgmtIp[SERVER_IP_MAX];
} serverNetGear;
solariStatus serverDbUpsertNetGear(serverDb *db, const serverNetGear *gear,
                                   uint64_t whenUnixMs);

/* ---- enrollment CRUD (Handoff §5 enrollment, §7.3) ---- */
typedef enum {
    ENROLL_TOKEN    = 0,
    ENROLL_PENDING  = 1,
    ENROLL_APPROVED = 2,
    ENROLL_REJECTED = 3,
    ENROLL_EXPIRED  = 4
} serverEnrollStatus;
typedef struct {
    uint64_t           enrId;
    char               host[SOLARI_FQDN_MAX];
    char               ip[SERVER_IP_MAX];
    solariRole         role;
    char               certFp[96];        /* colon-hex SHA-256, "" until CSR */
    serverEnrollStatus status;
    char               decidedBy[64];
} serverEnrollment;
/* Create an enrollment row (status TOKEN or PENDING). *enrId returns its id. */
solariStatus serverDbEnrollCreate(serverDb *db, const serverEnrollment *e,
                                  uint64_t whenUnixMs, uint64_t *enrId);
/* Attach a received CSR PEM + its fingerprint, moving TOKEN -> PENDING. */
solariStatus serverDbEnrollSetCsr(serverDb *db, uint64_t enrId,
                                  const char *csrPem, const char *certFp);
/* Decide an enrollment: status APPROVED/REJECTED + decidedBy + decidedAt.
 * On approval the caller nulls csrPem after signing. */
solariStatus serverDbEnrollDecide(serverDb *db, uint64_t enrId,
                                  serverEnrollStatus status, const char *decidedBy,
                                  uint64_t whenUnixMs);
/* Load one enrollment by id (csrPem is NOT returned here; use the CSR getter). */
solariStatus serverDbEnrollGet(serverDb *db, uint64_t enrId, serverEnrollment *out);
/* Fetch the held CSR PEM for signing into csrBuf; ERR_BUFFER_FULL if too small. */
solariStatus serverDbEnrollGetCsr(serverDb *db, uint64_t enrId,
                                  char *csrBuf, size_t cap);
/* Clear the stored CSR PEM after a successful sign (held only until signed). */
solariStatus serverDbEnrollClearCsr(serverDb *db, uint64_t enrId);

/* ---- build convergence (Handoff §5 buildArtifact; §7.3, /api/builds) ---- */
typedef struct {
    uint64_t buildId;
    char     arch[SOLARI_ARCH_MAX];
    char     os[SOLARI_OSNAME_MAX];
    char     version[SERVER_VERSION_MAX];
    char     channel[8];                 /* "stable"|"beta"|"dev"           */
    char     sha256[SERVER_SHA256_HEX_LEN + 1];
    uint64_t sizeBytes;
    uint32_t nodeCount;                  /* nodes converged to this build   */
} serverBuildRow;
/* Join buildArtifact against nodeConfig.appliedEpoch to compute per-build
 * convergence counts. Writes up to *count rows into out; updates *count. */
solariStatus serverDbBuildConvergence(serverDb *db, serverBuildRow *out,
                                      size_t *count);

/* ===================================================================== */
/* serverIngest.c - frame intake -> persistence (§9.1)                    */
/* ===================================================================== */

/* Validate a received frame: enforce that msgType is legal for the presenting
 * cert role (CN-derived) and dedup by (sourceNodeId, seqNo). Header sanity and
 * CRC are already checked by solariFrameParse. Returns ERR_AUTH_ROLE on a
 * role violation, ERR_CONN_RETRY (benign) on a recognised duplicate. */
solariStatus serverIngestValidate(serverContext *ctx,
                                  const solariFrameHeader *hdr,
                                  const char *peerCertCn);
/* Dispatch one validated frame to the right handler:
 *   CLIENT_REPORT/MONITOR_REPORT/WATCHDOG -> db writers
 *   DISCOVERY_ADVERT  -> serverDiscovery
 *   TOPOLOGY_REPORT   -> serverTopology
 *   SURVEY_RESP       -> reconcile (master)
 *   CONTROL_RESULT    -> convergence tracker (control)
 * Returns the handler's status; ERR_UNKNOWN_MSG for an unhandled type. */
solariStatus serverIngestDispatch(serverContext *ctx,
                                  const solariFrameHeader *hdr,
                                  const uint8_t *payload, size_t len);

/* ===================================================================== */
/* serverLease.c - primary/standby failover lease (§9.2)                  */
/* ===================================================================== */

/* Attempt to acquire/renew the lease. On success sets *state and, on a fresh
 * claim, advances *epoch. The single conditional UPDATE is the election (§2).
 * Called every cfg->leaseRenewSec. Returns ERR_LEASE_LOST if an active server
 * failed to renew (caller demotes). */
solariStatus serverLeaseTick(serverContext *ctx, serverRoleState *state,
                             uint64_t *epoch);
/* STANDBY->ACTIVE: bind ingest/control/pub sockets, resume DB writes.
 * ACTIVE->STANDBY: unbind, flush, go read-only. Idempotent. */
solariStatus serverApplyRole(serverContext *ctx, serverRoleState newState);
/* Build a PRIMARY_IS (0x41) reply payload naming the current lease holder, in
 * answer to a WHO_IS_PRIMARY (0x40). Writes the TLV payload + count. */
solariStatus serverLeaseAnswerWhoIsPrimary(serverContext *ctx,
                                           uint8_t *out, size_t cap,
                                           size_t *outLen, uint16_t *tlvCount);

/* ===================================================================== */
/* serverMaster.c - master coordination / session acceptance (§9.1)      */
/* ===================================================================== */

/* Accept a HELLO (0x01) session: upsert the node, then build a WELCOME (0x02)
 * payload returning the assigned schedule + config epoch. peerCertCn supplies
 * the cert CN for the node row. Writes the WELCOME TLV payload + count. */
solariStatus serverMasterAcceptHello(serverContext *ctx,
                                     const solariFrameHeader *hdr,
                                     const solariHello *hello, const char *peerCertCn,
                                     uint8_t *out, size_t cap,
                                     size_t *outLen, uint16_t *tlvCount);
/* Reconcile a SURVEY_RESP / overlapping monitor reports into authoritative
 * per-node/per-target current state and recompute derived health. */
solariStatus serverMasterReconcile(serverContext *ctx, uint64_t nodeId,
                                   const solariMonitorReport *rep);

/* ===================================================================== */
/* serverAlert.c - rule evaluation -> alertEvent (§9.1)                   */
/* ===================================================================== */

/* Evaluate host-scoped alert rules against an incoming client report; fire/
 * clear alertEvent rows via serverDb. `urgent` (SCP_FLAG_URGENT) takes the
 * immediate fast path instead of the batched window. */
solariStatus serverAlertEvalClient(serverContext *ctx, uint64_t nodeId,
                                   const solariClientReport *rep, bool urgent);
/* Evaluate probe-scoped rules against an incoming monitor report. */
solariStatus serverAlertEvalMonitor(serverContext *ctx, uint64_t nodeId,
                                    const solariMonitorReport *rep, bool urgent);

/* ===================================================================== */
/* serverControl.c - control-plane emit + result tracking (§9.1, §11)     */
/* ===================================================================== */

/* Build an SCP_MSG_CONTROL (0x30) frame for `targetNode` carrying `verb` and an
 * optional payload, stamped from ctx (sourceNodeId, seqNo, time). Writes the
 * full frame into out. correlationId is set so the node's CONTROL_RESULT can be
 * matched back. */
solariStatus serverControlBuild(serverContext *ctx, uint64_t targetNode,
                                solariCtrlVerb verb, uint64_t targetEpoch,
                                const uint8_t *payload, uint16_t payloadLen,
                                uint8_t *out, size_t cap, size_t *outLen);
/* Build an SCP_MSG_SURVEY (0x20) demanding an immediate report round. */
solariStatus serverControlBuildSurvey(serverContext *ctx,
                                      uint8_t *out, size_t cap, size_t *outLen);
/* Handle an inbound CONTROL_RESULT (0x31): parse the outcome, update the node's
 * convergence (appliedEpoch / lastResult) via serverDb. */
solariStatus serverControlOnResult(serverContext *ctx, uint64_t nodeId,
                                   uint32_t correlationId,
                                   const uint8_t *payload, size_t len);
/* Handle an inbound SURVEY_RESP (0x21): hand off to master reconcile. */
solariStatus serverControlOnSurveyResp(serverContext *ctx, uint64_t nodeId,
                                       const uint8_t *payload, size_t len);

/* ===================================================================== */
/* serverProvision.c - enrollment + lifecycle (Handoff §7.3, §7.4)        */
/* ===================================================================== */

/* Wipe-scope bitfield for CTRL_DECOMMISSION (mirrors TLV_LIFE_WIPE_SCOPE). */
enum {
    WIPE_CONFIG = 0x01,
    WIPE_CERTS  = 0x02,
    WIPE_SPOOL  = 0x04,
    WIPE_LOGS   = 0x08,
    WIPE_UNIT   = 0x10
};

/* Advance an enrollment through token->pending->approved/rejected. Approval
 * signs the CSR via the internal CA (through solariCtl) and upserts the node. */
solariStatus serverProvisionApprove(serverContext *ctx, uint64_t enrId,
                                    const char *operator_);
solariStatus serverProvisionReject(serverContext *ctx, uint64_t enrId,
                                   const char *operator_);

/* Emit CTRL_PROVISION (first-time bring-up) to a node: write config, converge
 * to targetEpoch. Idempotent. */
solariStatus serverProvisionNode(serverContext *ctx, uint64_t nodeId,
                                 uint64_t buildId, const char *configJson,
                                 uint64_t targetEpoch);

/* Begin a guarded decommission: generate a one-time confirm token, persist an
 * audit alertEvent row, and emit SCP_MSG_DECOMMISSION (0x32) carrying
 * CTRL_DECOMMISSION + the confirm token + wipeScope. *confirmToken returns the
 * issued token (the node must echo it). Requires the caller already enforced
 * operator RBAC + explicit confirm (§11). */
solariStatus serverProvisionDecommission(serverContext *ctx, uint64_t nodeId,
                                         uint32_t wipeScope, const char *operator_,
                                         uint64_t *confirmToken);
/* Finalize a decommission once the node's CONTROL_RESULT arrives (or on a
 * force-retire): set node.state='retired'. */
solariStatus serverProvisionRetire(serverContext *ctx, uint64_t nodeId);

/* Adopt a discovered entity as a live probe target (Handoff §7.4): resolve the
 * discovered row to a probe spec and send CTRL_ADOPT_TARGET to the owning
 * monitors (HRW-selected). Marks the discovered row 'adopting'/'adopted'. */
solariStatus serverProvisionAdoptTarget(serverContext *ctx, uint64_t discId,
                                        const char *probeSpec);
/* Dispatch an already-formed monitor probe spec as CTRL_ADOPT_TARGET. This does
 * not touch discovered/probe-target tables; callers persist intent first. */
solariStatus serverProvisionDispatchTarget(serverContext *ctx,
                                           const char *probeSpec);

/* ===================================================================== */
/* serverDiscovery.c - DISCOVERY_ADVERT consumer (Handoff §7.1)           */
/* ===================================================================== */

/* Consume an SCP_MSG_DISCOVERY_ADVERT (0x13) payload from `nodeId`: parse its
 * discovery TLVs, de-dupe against the node table, and upsert each entity into
 * 'discovered' (incrementing seenCount). Honors cfg->autoDiscover (gather vs
 * not) and cfg->autoEnroll (auto-issue enrollment vs hold for operator). */
solariStatus serverDiscoveryOnAdvert(serverContext *ctx, uint64_t nodeId,
                                     const uint8_t *payload, size_t len);
/* Operator action: promote a discovered candidate to monitored (adopt as probe
 * target, or open an enrollment, per the chosen role). Routes through
 * serverProvision. */
solariStatus serverDiscoveryAdopt(serverContext *ctx, uint64_t discId,
                                  const char *roleOrSpec);
/* Operator action: suppress a discovered candidate from the "new" list. */
solariStatus serverDiscoveryIgnore(serverContext *ctx, uint64_t discId);

/* ===================================================================== */
/* serverTopology.c - TOPOLOGY_REPORT consumer (Handoff §7.2)             */
/* ===================================================================== */

/* Consume an SCP_MSG_TOPOLOGY_REPORT (0x14) payload from `nodeId`: parse its
 * uplink / LLDP / segment TLVs and maintain networkGear + lldpEdge. */
solariStatus serverTopologyOnReport(serverContext *ctx, uint64_t nodeId,
                                    const uint8_t *payload, size_t len);

/* Deep managed-gear interrogation: LLDP-over-SNMP neighbour walk of `mgmtIp`
 * (behind SOLARI_WITH_SNMP), normalized into networkGear + lldpEdge rows. nodeId
 * is the SolariNet node that ran the walk (0 for an out-of-band collector). With
 * no SNMP backend built in the walk is a clean no-op returning ERR_PLATFORM. */
solariStatus serverTopologyDeepWalk(serverContext *ctx, uint64_t nodeId,
                                    const char *mgmtIp, const char *community,
                                    const char *segId);
/* Deep-walk every networkGear with an mgmtIp (driver for solariSnmpPoll --lldp).
 * Best-effort: one gear's failure does not abort the rest. */
solariStatus serverTopologyDeepWalkAll(serverContext *ctx, const char *defaultCommunity);

/* ===================================================================== */
/* solariCtl.c - operator bridge over a Unix domain socket (§9.1, §11.1)  */
/* ===================================================================== */

/* Open the operator bridge: bind the Unix domain socket at cfg->ctlSocket and
 * load the CA/cert material kept out of the web tier. *out gets the handle.
 * Returns ERR_PLATFORM on socket bind failure, ERR_TLS on bad CA/cert. */
solariStatus serverCtlOpen(const serverConfig *cfg, serverContext *ctx,
                           serverCtl **out);
/* Close the bridge and release the handle (NULL-safe). */
void         serverCtlClose(serverCtl *ctl);
/* Service pending operator requests on the Unix socket: read a request, validate
 * it (RBAC + confirm for destructive ops), re-emit the corresponding SCP control
 * frame through ctx, and write the reply. Non-blocking; processes what is ready.
 * Returns SOLARI_OK (including when nothing was ready). */
solariStatus serverCtlPoll(serverCtl *ctl);
/* Sign a CSR with the internal CA, returning the issued cert PEM into certBuf.
 * Holds the CA key material so it never reaches the web tier. ERR_BUFFER_FULL
 * if certBuf is too small, ERR_TLS on a signing failure. */
solariStatus serverCtlSignCsr(serverCtl *ctl, const char *csrPem,
                              char *certBuf, size_t cap);

#endif /* SOLARI_SERVER_H */
