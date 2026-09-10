#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../panelNet.h"

static unsigned tests;

static void check(bool condition) { assert(condition); ++tests; }

int main(void) {
  check(panelNetKnownHostType(PANEL_FT_SNAPSHOT));
  check(panelNetKnownHostType(PANEL_FT_PING));
  check(panelNetKnownHostType(PANEL_FT_HELLOREQ));
  check(panelNetKnownHostType(PANEL_FT_CONTROL));
  check(panelNetKnownHostType(PANEL_FT_PROVISION));
  check(!panelNetKnownHostType(PANEL_FT_STATE));
  check(!panelNetKnownHostType(0x7fu));

  check(!panelNetEpochPlausible(100u, 100u));
  check(!panelNetEpochPlausible(99u, 100u));
  check(panelNetEpochPlausible(101u, 100u));

  check(panelNetUseWifiTx(PANEL_NET_LINKED, true));
  check(!panelNetUseWifiTx(PANEL_NET_LINKED, false));
  check(!panelNetUseWifiTx(PANEL_NET_CONNECTING, true));
  check(!panelNetUseWifiTx(PANEL_NET_SUSPENDED, true));
  check(!panelNetUseWifiTx(PANEL_NET_OFF, false));

  printf("panelNetTest: %u assertions passed\n", tests);
  return 0;
}
