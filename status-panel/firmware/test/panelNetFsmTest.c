/*
 * panelNetFsmTest.c — host tests for WiFi policy and D13 arbitration state.
 *
 * Build and run: make -C firmware/test
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../panelLink.h"
#include "../panelNetFsm.h"

typedef struct {
  char actions[128];
  size_t actionCount;
  uint8_t randomByte;
  unsigned closeCount;
} FakeNet;

static int gFailures;

static void check(bool ok, const char *what) {
  if (!ok) {
    printf("FAIL: %s\n", what);
    gFailures++;
  } else {
    printf("ok:   %s\n", what);
  }
}

static void action(FakeNet *fake, char name) {
  if (fake->actionCount < sizeof(fake->actions))
    fake->actions[fake->actionCount++] = name;
}

static void fakeJoin(void *user) { action((FakeNet *)user, 'J'); }
static void fakeAddress(void *user) { action((FakeNet *)user, 'A'); }
static void fakeTime(void *user) { action((FakeNet *)user, 'N'); }
static void fakeTls(void *user) { action((FakeNet *)user, 'T'); }
static void fakeClose(void *user) {
  FakeNet *fake = (FakeNet *)user;
  fake->closeCount++;
  action(fake, 'C');
}
static uint8_t fakeRand(void *user) {
  return ((FakeNet *)user)->randomByte;
}

static PanelNetOps fakeOps(FakeNet *fake) {
  PanelNetOps ops;
  ops.startJoin = fakeJoin;
  ops.startAddressing = fakeAddress;
  ops.startTimeSync = fakeTime;
  ops.startTls = fakeTls;
  ops.close = fakeClose;
  ops.randByte = fakeRand;
  ops.user = fake;
  return ops;
}

static void driveLinked(PanelNetFsm *fsm, uint32_t ms, uint64_t epoch) {
  panelNetFsmPoll(fsm, ms);
  panelNetFsmJoinResult(fsm, true, ms);
  panelNetFsmPoll(fsm, ms);
  panelNetFsmAddressResult(fsm, true, ms);
  panelNetFsmPoll(fsm, ms);
  if (fsm->state == PANEL_NET_TIMESYNC) {
    panelNetFsmTimeResult(fsm, true, epoch, ms);
    panelNetFsmPoll(fsm, ms);
  }
  panelNetFsmTlsResult(fsm, true, ms);
}

/* D7: an unprovisioned unit is inert; after commit, SNTP must precede TLS. */
static void caseHappyPathAndTimeGate(void) {
  FakeNet fake;
  PanelNetFsm fsm;
  PanelNetOps ops;
  memset(&fake, 0, sizeof(fake));
  ops = fakeOps(&fake);
  panelNetFsmInit(&fsm, &ops, false, false, 0u, 1000u);
  panelNetFsmPoll(&fsm, 0u);
  check(fsm.state == PANEL_NET_OFF && fake.actionCount == 0u,
        "happy: unprovisioned stays OFF");

  panelNetFsmSetProvisioned(&fsm, true);
  driveLinked(&fsm, 10u, 1001u);
  check(fsm.state == PANEL_NET_LINKED, "happy: reaches LINKED after commit");
  check(fake.actionCount == 4u && memcmp(fake.actions, "JANT", 4u) == 0,
        "happy: join/address/SNTP/TLS ordering is exact");

  memset(&fake, 0, sizeof(fake));
  ops = fakeOps(&fake);
  panelNetFsmInit(&fsm, &ops, true, true, 1000u, 1000u);
  panelNetFsmPoll(&fsm, 0u);
  panelNetFsmJoinResult(&fsm, true, 0u);
  panelNetFsmPoll(&fsm, 0u);
  panelNetFsmAddressResult(&fsm, true, 0u);
  panelNetFsmPoll(&fsm, 0u);
  check(fsm.state == PANEL_NET_TIMESYNC &&
        memchr(fake.actions, 'T', fake.actionCount) == NULL,
        "D7: wall time equal to build epoch refuses TLS");
  panelNetFsmTimeResult(&fsm, true, 1000u, 1u);
  check(fsm.state == PANEL_NET_BACKOFF &&
        memchr(fake.actions, 'T', fake.actionCount) == NULL,
        "D7: implausible SNTP result backs off without TLS");
}

static void caseBackoff(void) {
  static const uint32_t bases[] = {
    1000u, 2000u, 4000u, 8000u, 16000u, 32000u, 60000u, 60000u
  };
  FakeNet fake;
  PanelNetFsm fsm;
  PanelNetOps ops;
  uint32_t now = 0u;
  size_t i;
  memset(&fake, 0, sizeof(fake));
  fake.randomByte = 255u;
  ops = fakeOps(&fake);
  panelNetFsmInit(&fsm, &ops, true, false, 0u, 1000u);

  for (i = 0u; i < sizeof(bases) / sizeof(bases[0]); ++i) {
    uint32_t lower = bases[i];
    uint32_t upper = bases[i] + bases[i] / 4u;
    if (bases[i] == PANEL_NET_BACKOFF_MAX_MS) {
      lower = bases[i] - bases[i] / 4u;
      upper = bases[i];
    }
    if (upper > PANEL_NET_BACKOFF_MAX_MS) upper = PANEL_NET_BACKOFF_MAX_MS;
    panelNetFsmPoll(&fsm, now);
    panelNetFsmJoinResult(&fsm, false, now);
    check(fsm.lastBackoffMs >= lower && fsm.lastBackoffMs <= upper,
          "backoff: exponential delay stays inside jitter bound");
    now += fsm.lastBackoffMs;
  }
  fake.randomByte = 0u;
  panelNetFsmPoll(&fsm, now);
  panelNetFsmJoinResult(&fsm, false, now);
  check(fsm.lastBackoffMs == PANEL_NET_BACKOFF_MAX_MS,
        "backoff: jittered delay can reach but never exceed 60 seconds");
  now += fsm.lastBackoffMs;

  panelNetFsmPoll(&fsm, now);
  panelNetFsmJoinResult(&fsm, true, now);
  panelNetFsmPoll(&fsm, now);
  panelNetFsmAddressResult(&fsm, true, now);
  panelNetFsmPoll(&fsm, now);
  panelNetFsmTimeResult(&fsm, true, 1001u, now);
  panelNetFsmPoll(&fsm, now);
  panelNetFsmTlsResult(&fsm, true, now);
  fake.randomByte = 255u;
  panelNetFsmServerClosed(&fsm, now);
  check(fsm.lastBackoffMs == 1250u,
        "backoff: successful TLS resets next delay to 1 s base + jitter");
}

static void caseFailureRetryStates(void) {
  FakeNet fake;
  PanelNetFsm fsm;
  PanelNetOps ops;
  memset(&fake, 0, sizeof(fake));
  ops = fakeOps(&fake);

  panelNetFsmInit(&fsm, &ops, true, false, 0u, 1000u);
  panelNetFsmPoll(&fsm, 0u);
  panelNetFsmJoinResult(&fsm, true, 0u);
  panelNetFsmPoll(&fsm, 0u);
  panelNetFsmAddressResult(&fsm, false, 0u);
  check(fsm.state == PANEL_NET_BACKOFF &&
        fsm.retryState == PANEL_NET_ADDRESSING,
        "failures: DHCP failure retries ADDRESSING after backoff");

  panelNetFsmPoll(&fsm, fsm.deadlineMs);
  panelNetFsmAddressResult(&fsm, true, fsm.deadlineMs);
  panelNetFsmPoll(&fsm, fsm.deadlineMs);
  panelNetFsmTimeResult(&fsm, false, 0u, fsm.deadlineMs);
  check(fsm.state == PANEL_NET_BACKOFF && fsm.retryState == PANEL_NET_TIMESYNC,
        "failures: SNTP failure retries TIMESYNC after backoff");

  panelNetFsmPoll(&fsm, fsm.deadlineMs);
  panelNetFsmTimeResult(&fsm, true, 1001u, fsm.deadlineMs);
  panelNetFsmPoll(&fsm, fsm.deadlineMs);
  panelNetFsmTlsResult(&fsm, false, fsm.deadlineMs);
  check(fsm.state == PANEL_NET_BACKOFF &&
        fsm.retryState == PANEL_NET_CONNECTING,
        "failures: TLS failure retries CONNECTING after backoff");

  panelNetFsmPoll(&fsm, fsm.deadlineMs);
  panelNetFsmTlsResult(&fsm, true, fsm.deadlineMs);
  panelNetFsmServerClosed(&fsm, fsm.deadlineMs);
  check(fsm.state == PANEL_NET_BACKOFF &&
        fsm.retryState == PANEL_NET_CONNECTING,
        "failures: server close retries CONNECTING after backoff");

  panelNetFsmPoll(&fsm, fsm.deadlineMs);
  panelNetFsmApLost(&fsm, fsm.deadlineMs);
  check(fsm.state == PANEL_NET_BACKOFF && fsm.retryState == PANEL_NET_JOINING,
        "failures: AP loss retries JOINING after backoff");
}

static void caseUsbArbitration(void) {
  FakeNet fake;
  PanelNetFsm fsm;
  PanelNetOps ops;
  memset(&fake, 0, sizeof(fake));
  ops = fakeOps(&fake);
  panelNetFsmInit(&fsm, &ops, true, true, 2000u, 1000u);
  driveLinked(&fsm, 0u, 2000u);

  panelNetFsmUsbFrameSeen(&fsm, false, 100u);
  check(fsm.state == PANEL_NET_LINKED && fake.closeCount == 0u,
        "D14: CRC-valid unknown USB type does not qualify");
  panelNetFsmUsbFrameSeen(&fsm, true, 200u);
  check(fsm.state == PANEL_NET_SUSPENDED && fake.closeCount == 1u,
        "D14: first known USB frame closes WiFi immediately");
  panelNetFsmPoll(&fsm, 15199u);
  check(fsm.state == PANEL_NET_SUSPENDED,
        "D14: WiFi remains suspended before 15 s silence");
  panelNetFsmPoll(&fsm, 15200u);
  check(fsm.state == PANEL_NET_JOINING &&
        fake.actions[fake.actionCount - 1u] == 'J',
        "D14: WiFi resumes at exactly 15 s silence");

  panelNetFsmUsbFrameSeen(&fsm, true, 20000u);
  check(fsm.state == PANEL_NET_SUSPENDED,
        "D14: later qualifying USB frame suspends again");
  panelNetFsmCdcDisconnected(&fsm, 20001u);
  panelNetFsmPoll(&fsm, 20001u);
  check(fsm.state == PANEL_NET_JOINING &&
        fake.actions[fake.actionCount - 1u] == 'J',
        "D14: CDC disconnect bypasses the silence delay");

  memset(&fake, 0, sizeof(fake));
  ops = fakeOps(&fake);
  panelNetFsmInit(&fsm, &ops, false, false, 0u, 1000u);
  panelNetFsmUsbFrameSeen(&fsm, true, 30000u);
  panelNetFsmSetProvisioned(&fsm, true);
  panelNetFsmPoll(&fsm, 30000u);
  check(fsm.state == PANEL_NET_SUSPENDED && fake.actionCount == 0u,
        "D14: PROVISION commit does not start WiFi under active USB");
}

static void casePerTransportSequence(void) {
  PanelLink link;
  bool resynced = false;
  panelLinkInit(&link, 0u);
  check(panelLinkAcceptSnapshot(&link, PANEL_LINK_SERIAL, 500u, 1000u,
                                &resynced),
        "D13: initial USB snapshot applies");
  panelLinkSetActiveTransport(&link, PANEL_LINK_WIFI);
  check(panelLinkAcceptSnapshot(&link, PANEL_LINK_WIFI, 3u, 2000u, &resynced),
        "D13: lower WiFi seq is adopted on USB-to-WiFi switch");
  check(!panelLinkAcceptSnapshot(&link, PANEL_LINK_SERIAL, 501u, 2100u,
                                 &resynced),
        "D13: buffered frame from losing USB transport is discarded");
  panelLinkSetActiveTransport(&link, PANEL_LINK_SERIAL);
  check(panelLinkAcceptSnapshot(&link, PANEL_LINK_SERIAL, 500u, 3000u,
                                &resynced),
        "D13: equal USB seq is adopted on WiFi-to-USB switch");
  panelLinkSetActiveTransport(&link, PANEL_LINK_WIFI);
  check(panelLinkAcceptSnapshot(&link, PANEL_LINK_WIFI, 65535u, 4000u,
                                &resynced),
        "D13: wrap-boundary WiFi seq is explicitly adopted");
  check(panelLinkAcceptSnapshot(&link, PANEL_LINK_WIFI, 0u, 4100u, &resynced),
        "D13: normal RFC1982 wrap proceeds within one transport");
  check(panelLinkPoll(&link, 4100u) == PANEL_LINK_NO_CHANGE,
        "D14: fresh switch snapshots do not emit LINKLOST/LINKBACK");

  panelLinkResetTransportSeq(&link, PANEL_LINK_WIFI);
  check(panelLinkAcceptSnapshot(&link, PANEL_LINK_WIFI, 0u, 4200u, &resynced),
        "D13: new WiFi connection explicitly adopts equal seq");
  check(!panelLinkAcceptSnapshot(&link, PANEL_LINK_WIFI, 0u, 40000u,
                                 &resynced),
        "D13: timed seq escape hatch remains disabled on WiFi");
}

int main(void) {
  caseHappyPathAndTimeGate();
  caseBackoff();
  caseFailureRetryStates();
  caseUsbArbitration();
  casePerTransportSequence();
  if (gFailures != 0) {
    printf("\n%d FAILURE(S)\n", gFailures);
    return 1;
  }
  printf("\nall panelNetFsm cases pass\n");
  return 0;
}
