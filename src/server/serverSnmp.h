/*
 * serverSnmp.h - SNMP polling tier: managed-gear interface metrics (§ monitoring).
 *
 * The server polls each networkGear's IF-MIB counters (ifHCIn/OutOctets,
 * ifOperStatus, ifName) by NUMERIC OID via the net-snmp CLI (snmpbulkwalk), so
 * no MIB files and no libnetsnmp link are required. Per-interface throughput
 * rates are computed from octet deltas between samples and written to
 * gearInterfaceCurrent + gearInterfaceHistory.
 *
 * The parsing/normalization is pure and unit-tested; the actual walk is behind
 * SOLARI_WITH_SNMP (a runtime dependency on the net-snmp CLI only). With the
 * macro undefined the walk returns ERR_PLATFORM and the poll is a no-op.
 */
#ifndef SOLARI_SERVER_SNMP_H
#define SOLARI_SERVER_SNMP_H

#include "server.h"

#define SERVER_SNMP_MAX_IFACES 256

typedef struct {
    int                ifIndex;
    char               name[96];
    unsigned long long inOctets;
    unsigned long long outOctets;
    int                operStatus;   /* IF-MIB ifOperStatus: 1=up, 2=down, ... */
    int                haveName, haveIn, haveOut, haveStatus;
} serverSnmpIface;

/* Parse one `snmpbulkwalk -Onq` output line ("<numeric-oid> <value>") into the
 * trailing OID label (the ifIndex) and the value string (quotes stripped).
 * Pure; returns 1 on success, 0 on a malformed line. Unit-tested. */
int serverSnmpParseWalkLine(const char *line, int *ifIndex, char *val, size_t valCap);

/* Merge a parsed (oid-kind, ifIndex, value) tuple into the ifaces[] table,
 * keyed by ifIndex. `kind` is one of the SERVER_SNMP_OID_* selectors. Pure. */
typedef enum { SNMP_OID_NAME = 0, SNMP_OID_IN, SNMP_OID_OUT, SNMP_OID_STATUS } serverSnmpOidKind;
int serverSnmpMergeIface(serverSnmpIface *ifaces, size_t *count, size_t cap,
                         serverSnmpOidKind kind, int ifIndex, const char *val);

/* Poll one gear over SNMP and persist current + history (rates computed vs the
 * previous gearInterfaceCurrent sample). community may be NULL/"" -> caller's
 * default. Returns ERR_PLATFORM when built without SOLARI_WITH_SNMP. */
solariStatus serverSnmpPollGear(serverContext *ctx, const char *gearId,
                                const char *mgmtIp, const char *community);

/* Poll every networkGear that has an mgmtIp. *polledOut (optional) gets the
 * count of gear successfully polled. Best-effort: one gear's failure does not
 * abort the rest. */
solariStatus serverSnmpPollAll(serverContext *ctx, const char *defaultCommunity,
                               size_t *polledOut);

#endif /* SOLARI_SERVER_SNMP_H */
