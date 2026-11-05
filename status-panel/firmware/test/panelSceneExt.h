/* panelSceneExt.h — decode the CONTRACT-V2 §4 JSON keys into a PanelSnapshot,
 * shared by panelSim.c and panelParityTest.c so both read scenes identically.
 * Include after testJson.h. Test-only; not firmware. */

#ifndef PANEL_SCENE_EXT_H
#define PANEL_SCENE_EXT_H

#include <string.h>
#include "../../protocol.h"
#include "testJson.h"

static uint8_t sceneState(const JVal *v) {
  const char *w;
  if (v && v->type == J_NUM) return (uint8_t)v->num;
  w = jStr(v, "unknown");
  if (!strcmp(w, "up") || !strcmp(w, "ok")) return PANEL_ST_OK;
  if (!strcmp(w, "degraded")) return PANEL_ST_DEGRADED;
  if (!strcmp(w, "down")) return PANEL_ST_DOWN;
  if (!strcmp(w, "maint")) return PANEL_ST_MAINT;
  return PANEL_ST_UNKNOWN;
}

static uint8_t sceneKind(const JVal *v) {
  const char *w;
  if (v && v->type == J_NUM) return (uint8_t)v->num;
  w = jStr(v, "host");
  if (!strcmp(w, "wan")) return PANEL_LK_WAN;
  if (!strcmp(w, "trunk")) return PANEL_LK_TRUNK;
  if (!strcmp(w, "wifi")) return PANEL_LK_WIFI;
  return PANEL_LK_HOST;
}

/* null or absent = unmeasured, which is not zero (CONTRACT-V2 §3). */
static uint32_t sceneRate(const JVal *v) {
  return v && v->type == J_NUM ? (uint32_t)v->num : PANEL_RATE_NONE;
}

static void sceneCopy(char *dst, size_t cap, const JVal *v) {
  const char *s = jStr(v, "");
  size_t n = strlen(s);
  if (n >= cap) n = cap - 1;
  memcpy(dst, s, n);
  dst[n] = '\0';
}

static void sceneDecodeExt(const JVal *s, PanelSnapshot *snap) {
  const JVal *o, *a;
  int i, n;

  o = jGet(s, "links");
  if (o) {
    snap->hasLinks = 1u;
    n = jLen(o);
    if (n > (int)PANEL_MAX_LINKS) n = (int)PANEL_MAX_LINKS;
    for (i = 0; i < n; i++) {
      const JVal *l = jAt(o, i);
      sceneCopy(snap->links[i].name, sizeof snap->links[i].name, jGet(l, "name"));
      snap->links[i].kind = sceneKind(jGet(l, "kind"));
      snap->links[i].state = sceneState(jGet(l, "state"));
      a = jGet(l, "parent");
      snap->links[i].parent = a && a->type == J_NUM ? (uint8_t)a->num : 0xFFu;
      snap->links[i].upKbps = sceneRate(jGet(l, "upKbps"));
      snap->links[i].downKbps = sceneRate(jGet(l, "downKbps"));
    }
    snap->linkCount = (uint8_t)n;
  }

  o = jGet(s, "systems");
  n = jLen(o);
  if (n > (int)PANEL_MAX_SYSTEMS) n = (int)PANEL_MAX_SYSTEMS;
  for (i = 0; i < n; i++) {
    const JVal *y = jAt(o, i);
    if (!jGet(y, "memPct") && !jGet(y, "rxKbps")) continue;
    snap->hasSysx = 1u;
    a = jGet(y, "memPct");
    snap->sysx[i].memPct = a && a->type == J_NUM ? (uint8_t)a->num : 0xFFu;
    a = jGet(y, "diskPct");
    snap->sysx[i].diskPct = a && a->type == J_NUM ? (uint8_t)a->num : 0xFFu;
    snap->sysx[i].faults = (uint8_t)jNum(jGet(y, "faults"), 0.0);
    snap->sysx[i].rxKbps = sceneRate(jGet(y, "rxKbps"));
    snap->sysx[i].txKbps = sceneRate(jGet(y, "txKbps"));
  }

  o = jGet(s, "alertList");
  if (o) {
    snap->hasAlerts = 1u;
    snap->alertTotal = (uint8_t)jNum(jGet(o, "total"), 0.0);
    a = jGet(o, "faultCounts");
    for (i = 0; i < (int)PANEL_FAULT_CODES && i < jLen(a); i++)
      snap->faultCounts[i] = (uint8_t)jNum(jAt(a, i), 0.0);
    a = jGet(o, "items");
    n = jLen(a);
    if (n > (int)PANEL_MAX_ALERTS) n = (int)PANEL_MAX_ALERTS;
    for (i = 0; i < n; i++) {
      const JVal *e = jAt(a, i);
      snap->alerts[i].severity = (uint8_t)jNum(jGet(e, "severity"), 0.0);
      snap->alerts[i].code = (uint8_t)jNum(jGet(e, "code"), 0.0);
      snap->alerts[i].ageMin = (uint16_t)jNum(jGet(e, "ageMin"), 0.0);
      sceneCopy(snap->alerts[i].host, sizeof snap->alerts[i].host, jGet(e, "host"));
      sceneCopy(snap->alerts[i].msg, sizeof snap->alerts[i].msg, jGet(e, "msg"));
    }
    snap->alertCount = (uint8_t)n;
  }

  o = jGet(s, "probes");
  if (o) {
    snap->hasProbes = 1u;
    n = jLen(o);
    if (n > (int)PANEL_MAX_PROBES) n = (int)PANEL_MAX_PROBES;
    for (i = 0; i < n; i++) {
      const JVal *p = jAt(o, i);
      sceneCopy(snap->probes[i].name, sizeof snap->probes[i].name, jGet(p, "name"));
      snap->probes[i].rttTenthMs = (uint16_t)jNum(jGet(p, "rttTenthMs"), 0.0);
      snap->probes[i].lossPermille = (uint16_t)jNum(jGet(p, "lossPermille"), 0.0);
      snap->probes[i].state = sceneState(jGet(p, "state"));
    }
    snap->probeCount = (uint8_t)n;
  }

  o = jGet(s, "popTail");
  n = jLen(o);
  if (n > (int)PANEL_MAX_POP) n = (int)PANEL_MAX_POP;
  for (i = 0; i < n; i++) snap->pop[i] = (uint8_t)jNum(jAt(o, i), 0.0);
  snap->popCount = (uint8_t)n;

  o = jGet(s, "alertHeat");
  if (o) {
    snap->hasHeat = 1u;
    for (i = 0; i < (int)PANEL_HEAT_LEN; i++) {
      snap->heatWarn[i] = (uint8_t)jNum(jAt(jGet(o, "warn"), i), 0.0);
      snap->heatCrit[i] = (uint8_t)jNum(jAt(jGet(o, "crit"), i), 0.0);
    }
  }

  o = jGet(s, "totals");
  if (o) {
    snap->hasTotals = 1u;
    snap->wanUpKbps = sceneRate(jGet(o, "wanUpKbps"));
    snap->wanDownKbps = sceneRate(jGet(o, "wanDownKbps"));
    snap->lanUpKbps = sceneRate(jGet(o, "lanUpKbps"));
    snap->lanDownKbps = sceneRate(jGet(o, "lanDownKbps"));
  }
}

#endif /* PANEL_SCENE_EXT_H */
