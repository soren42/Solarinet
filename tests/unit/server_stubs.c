/*
 * server_stubs.c - weak no-op definitions of the server's cross-subsystem
 * functions, so a unit test can #include exactly ONE subsystem .c (to reach its
 * pure static helpers) and still link without dragging in the whole
 * libservercore.a / a live MariaDB.
 *
 * Every symbol here is declared __attribute__((weak)): if the #included
 * subsystem .c (or any other linked object) provides a real definition, that
 * strong definition wins; the weak stub only fills the link gaps for the
 * external server* functions the included TU references in its NON-pure entry
 * points (which the tests never call). This keeps the pure-helper tests honest -
 * they exercise the real static code, not these stubs.
 */
#include "server.h"
#include "serverAssets.h"

#define WEAK __attribute__((weak))

/* ---- serverDb.c surface ---- */
WEAK solariStatus serverDbOpen(const serverConfig *cfg, serverDb **out)
{ (void)cfg; if (out) *out = 0; return ERR_DB; }
WEAK void serverDbClose(serverDb *db) { (void)db; }
WEAK solariStatus serverDbPing(serverDb *db) { (void)db; return ERR_DB; }
WEAK solariStatus serverDbApplyScript(serverDb *db, const char *p)
{ (void)db; (void)p; return ERR_DB; }
WEAK solariStatus serverDbUpsertNode(serverDb *db, uint64_t n, solariRole r,
    const char *a, const char *b, const char *c, const char *d, uint64_t e)
{ (void)db;(void)n;(void)r;(void)a;(void)b;(void)c;(void)d;(void)e; return ERR_DB; }
WEAK solariStatus serverDbTouchNode(serverDb *db, uint64_t n, uint64_t w)
{ (void)db;(void)n;(void)w; return ERR_DB; }
WEAK solariStatus serverDbSetNodeState(serverDb *db, uint64_t n, const char *s)
{ (void)db;(void)n;(void)s; return ERR_DB; }
WEAK solariStatus serverDbWriteClientReport(serverContext *c, uint64_t n,
    const solariClientReport *r) { (void)c;(void)n;(void)r; return ERR_DB; }
WEAK solariStatus serverDbWriteMonitorReport(serverContext *c, uint64_t n,
    const solariMonitorReport *r) { (void)c;(void)n;(void)r; return ERR_DB; }
WEAK solariStatus serverDbWriteAlertEvent(serverDb *db, int rid, uint64_t n,
    const char *t, const char *sev, const char *d, uint64_t f)
{ (void)db;(void)rid;(void)n;(void)t;(void)sev;(void)d;(void)f; return ERR_DB; }
WEAK solariStatus serverDbLoadAlertRules(serverDb *db, const char *s,
    serverAlertRule *o, size_t *c)
{ (void)db;(void)s;(void)o; if (c) *c = 0; return ERR_DB; }
WEAK solariStatus serverDbSetNodeTargetEpoch(serverDb *db, uint64_t n,
    uint64_t e, const char *j) { (void)db;(void)n;(void)e;(void)j; return ERR_DB; }
WEAK solariStatus serverDbSetNodeApplied(serverDb *db, uint64_t n, uint64_t e,
    const char *r, uint64_t w) { (void)db;(void)n;(void)e;(void)r;(void)w; return ERR_DB; }
WEAK solariStatus serverDbLeaseClaim(serverDb *db, uint64_t s, uint32_t t,
    bool *won, uint64_t *e) { (void)db;(void)s;(void)t; if (won)*won=false; if(e)*e=0; return ERR_DB; }
WEAK solariStatus serverDbLeaseRead(serverDb *db, uint64_t *h, uint64_t *e)
{ (void)db; if(h)*h=0; if(e)*e=0; return ERR_DB; }
WEAK solariStatus serverDbUpsertDiscovered(serverDb *db, const serverDiscEntity *e,
    uint64_t w, uint64_t *id) { (void)db;(void)e;(void)w; if(id)*id=0; return ERR_DB; }
WEAK solariStatus serverDbSetDiscoveredStatus(serverDb *db, uint64_t id,
    const char *s) { (void)db;(void)id;(void)s; return ERR_DB; }
WEAK solariStatus serverDbDiscoveredIsKnown(serverDb *db, const char *ip,
    const char *k, bool *ex) { (void)db;(void)ip;(void)k; if(ex)*ex=false; return ERR_DB; }
WEAK solariStatus serverDbGetDiscovered(serverDb *db, uint64_t id,
    serverDiscEntity *o) { (void)db;(void)id;(void)o; return ERR_DB; }
WEAK solariStatus serverDbWriteLldpEdge(serverDb *db, const serverLldpEdge *e,
    uint64_t w) { (void)db;(void)e;(void)w; return ERR_DB; }
WEAK solariStatus serverDbUpsertNetGear(serverDb *db, const serverNetGear *g,
    uint64_t w) { (void)db;(void)g;(void)w; return ERR_DB; }
WEAK solariStatus serverDbEnrollCreate(serverDb *db, const serverEnrollment *e,
    uint64_t w, uint64_t *id) { (void)db;(void)e;(void)w; if(id)*id=0; return ERR_DB; }
WEAK solariStatus serverDbEnrollSetCsr(serverDb *db, uint64_t id,
    const char *p, const char *f) { (void)db;(void)id;(void)p;(void)f; return ERR_DB; }
WEAK solariStatus serverDbEnrollDecide(serverDb *db, uint64_t id,
    serverEnrollStatus s, const char *by, uint64_t w)
{ (void)db;(void)id;(void)s;(void)by;(void)w; return ERR_DB; }
WEAK solariStatus serverDbEnrollGet(serverDb *db, uint64_t id, serverEnrollment *o)
{ (void)db;(void)id;(void)o; return ERR_DB; }
WEAK solariStatus serverDbEnrollGetCsr(serverDb *db, uint64_t id, char *b, size_t c)
{ (void)db;(void)id; if(b&&c)b[0]='\0'; return ERR_DB; }
WEAK solariStatus serverDbEnrollClearCsr(serverDb *db, uint64_t id)
{ (void)db;(void)id; return ERR_DB; }
WEAK solariStatus serverDbBuildConvergence(serverDb *db, serverBuildRow *o, size_t *c)
{ (void)db;(void)o; if(c)*c=0; return ERR_DB; }

/* ---- serverIngest dispatch targets ---- */
WEAK solariStatus serverAlertEvalClient(serverContext *c, uint64_t n,
    const solariClientReport *r, bool u) { (void)c;(void)n;(void)r;(void)u; return SOLARI_OK; }
WEAK solariStatus serverAlertEvalMonitor(serverContext *c, uint64_t n,
    const solariMonitorReport *r, bool u) { (void)c;(void)n;(void)r;(void)u; return SOLARI_OK; }
WEAK solariStatus serverMasterReconcile(serverContext *c, uint64_t n,
    const solariMonitorReport *r) { (void)c;(void)n;(void)r; return SOLARI_OK; }
WEAK solariStatus serverControlOnResult(serverContext *c, uint64_t n, uint32_t cor,
    const uint8_t *p, size_t l) { (void)c;(void)n;(void)cor;(void)p;(void)l; return SOLARI_OK; }
WEAK solariStatus serverControlOnSurveyResp(serverContext *c, uint64_t n,
    const uint8_t *p, size_t l) { (void)c;(void)n;(void)p;(void)l; return SOLARI_OK; }
WEAK solariStatus serverControlBuild(serverContext *c, uint64_t tn,
    solariCtrlVerb v, uint64_t te, const uint8_t *p, uint16_t pl,
    uint8_t *o, size_t cap, size_t *ol)
{ (void)c;(void)tn;(void)v;(void)te;(void)p;(void)pl;(void)o;(void)cap; if(ol)*ol=0; return SOLARI_OK; }
WEAK solariStatus serverControlBuildSurvey(serverContext *c,
    uint8_t *o, size_t cap, size_t *ol) { (void)c;(void)o;(void)cap; if(ol)*ol=0; return SOLARI_OK; }
WEAK solariStatus serverDiscoveryOnAdvert(serverContext *c, uint64_t n,
    const uint8_t *p, size_t l) { (void)c;(void)n;(void)p;(void)l; return SOLARI_OK; }
WEAK solariStatus serverDiscoveryAdopt(serverContext *c, uint64_t id, const char *r)
{ (void)c;(void)id;(void)r; return SOLARI_OK; }
WEAK solariStatus serverDiscoveryIgnore(serverContext *c, uint64_t id)
{ (void)c;(void)id; return SOLARI_OK; }
WEAK solariStatus serverTopologyOnReport(serverContext *c, uint64_t n,
    const uint8_t *p, size_t l) { (void)c;(void)n;(void)p;(void)l; return SOLARI_OK; }

/* ---- serverProvision + ctl signing ---- */
WEAK solariStatus serverProvisionApprove(serverContext *c, uint64_t id, const char *op)
{ (void)c;(void)id;(void)op; return SOLARI_OK; }
WEAK solariStatus serverProvisionReject(serverContext *c, uint64_t id, const char *op)
{ (void)c;(void)id;(void)op; return SOLARI_OK; }
WEAK solariStatus serverProvisionNode(serverContext *c, uint64_t n, uint64_t b,
    const char *j, uint64_t te) { (void)c;(void)n;(void)b;(void)j;(void)te; return SOLARI_OK; }
WEAK solariStatus serverProvisionDecommission(serverContext *c, uint64_t n,
    uint32_t ws, const char *op, uint64_t *tok)
{ (void)c;(void)n;(void)ws;(void)op; if(tok)*tok=0; return SOLARI_OK; }
WEAK solariStatus serverProvisionRetire(serverContext *c, uint64_t n)
{ (void)c;(void)n; return SOLARI_OK; }
WEAK solariStatus serverProvisionAdoptTarget(serverContext *c, uint64_t id, const char *s)
{ (void)c;(void)id;(void)s; return SOLARI_OK; }
WEAK solariStatus serverScanRun(serverContext *c, const char *cidr,
    const char *ports, size_t *found)
{ (void)c;(void)cidr;(void)ports; if(found)*found=0; return SOLARI_OK; }
WEAK solariStatus serverDbUpsertProbeTarget(serverDb *db, const char *tid,
    const char *host, int port, const char *proto, int repl,
    const char *label, const char *seg, uint64_t asset)
{ (void)db;(void)tid;(void)host;(void)port;(void)proto;(void)repl;(void)label;(void)seg;(void)asset; return SOLARI_OK; }
WEAK solariStatus serverDbDeleteProbeTarget(serverDb *db, const char *tid)
{ (void)db;(void)tid; return SOLARI_OK; }
WEAK solariStatus serverDbUpsertAsset(serverDb *db, const char *ip, const char *host,
    const char *dn, const char *cls, uint64_t pool, const char *tags,
    const char *notes, bool mon, uint64_t *aid)
{ (void)db;(void)ip;(void)host;(void)dn;(void)cls;(void)pool;(void)tags;(void)notes;(void)mon; if(aid)*aid=0; return SOLARI_OK; }
WEAK solariStatus serverDbGetAssetIdByIp(serverDb *db, const char *ip, uint64_t *aid)
{ (void)db;(void)ip; if(aid)*aid=0; return SOLARI_OK; }
WEAK solariStatus serverDbCreatePool(serverDb *db, const char *n, const char *d,
    const char *c, uint64_t *pid)
{ (void)db;(void)n;(void)d;(void)c; if(pid)*pid=0; return SOLARI_OK; }
WEAK solariStatus serverDbUpdatePool(serverDb *db, uint64_t pid, const char *n,
    const char *d, const char *c)
{ (void)db;(void)pid;(void)n;(void)d;(void)c; return SOLARI_OK; }
WEAK solariStatus serverDbSetGlobalConfig(serverDb *db, const char *cfg,
    const char *by, uint64_t *epoch)
{ (void)db;(void)cfg;(void)by; if(epoch)*epoch=0; return SOLARI_OK; }
WEAK solariStatus serverDbUpdateAlertRule(serverDb *db, const serverAlertRuleEdit *e)
{ (void)db;(void)e; return SOLARI_OK; }
WEAK solariStatus serverAssetsAdopt(serverContext *c, const serverAdoptOpts *o, uint64_t *aid)
{ (void)c;(void)o; if(aid)*aid=0; return SOLARI_OK; }
WEAK solariStatus serverAssetsSetMeta(serverContext *c, const char *ip, const char *dn,
    const char *cls, uint64_t pool, const char *tags, const char *notes, bool mon)
{ (void)c;(void)ip;(void)dn;(void)cls;(void)pool;(void)tags;(void)notes;(void)mon; return SOLARI_OK; }
WEAK solariStatus serverCtlSignCsr(serverCtl *ctl, const char *csr, char *b, size_t c)
{ (void)ctl;(void)csr; if(b&&c)b[0]='\0'; return ERR_TLS; }
