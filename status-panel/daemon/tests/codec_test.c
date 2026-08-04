#include "../../protocol.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int panelParseSnapshotForTest(const char *text, PanelSnapshot *snapshot);
int panelForwardCommandsForTest(int fd, const char *text, int stateSeen);

static int frames;
static int controlFrames, stateFrames;
static PanelSnapshot received;

static void receive(uint8_t type, const uint8_t *payload, size_t len, void *user) {
  (void)user;
  if (type == PANEL_FT_SNAPSHOT && panelDecodeSnapshot(payload, len, &received) == 0) ++frames;
  if (type == PANEL_FT_CONTROL) ++controlFrames;
  if (type == PANEL_FT_STATE) ++stateFrames;
}

static int require(int condition, const char *message) {
  if (!condition) { fprintf(stderr, "%s\n", message); return 1; }
  return 0;
}

/* Callback for the forwardCommands pipe test: counts CONTROL frames and
 * decodes the cmdId of the last one into *user. */
static void receiveControl(uint8_t type, const uint8_t *payload, size_t len, void *user) {
  uint8_t k, a;
  if (type == PANEL_FT_CONTROL) {
    ++controlFrames;
    (void)panelDecodeControl(payload, len, (uint32_t *)user, &k, &a);
  }
}

static size_t snapshotFrame(uint16_t seq, uint8_t *frame) {
  PanelSnapshot sent;
  uint8_t payload[PANEL_MAX_PAYLOAD];
  size_t payloadLen;
  memset(&sent, 0, sizeof(sent));
  sent.ts=42u; sent.seq=seq; sent.score=140u; sent.alarmActive=1u;
  sent.poolCount=1u; sent.systemCount=1u; sent.hasTopAlert=1u;
  strcpy(sent.pools[0].name,"core"); sent.pools[0].total=2u;
  sent.systems[0].pool=0u; strcpy(sent.systems[0].name,"xenon");
  sent.topAlert.alertId=3u; strcpy(sent.topAlert.subject,"bind down");
  payloadLen=panelEncodeSnapshot(&sent,payload,sizeof(payload));
  return panelEncodeFrame(PANEL_FT_SNAPSHOT,payload,payloadLen,frame,PANEL_HDR_SIZE+PANEL_MAX_PAYLOAD+PANEL_CRC_SIZE);
}

int main(void) {
  PanelParser parser;
  PanelSnapshot lastGood;
  uint8_t frame[PANEL_HDR_SIZE+PANEL_MAX_PAYLOAD+PANEL_CRC_SIZE];
  uint8_t control[PANEL_CONTROL_SIZE], state[PANEL_STATE_SIZE];
  uint8_t stream[4u*(PANEL_HDR_SIZE+PANEL_MAX_PAYLOAD+PANEL_CRC_SIZE)+16u];
  size_t frameLen, offset, i;
  uint32_t cmdId, ackedEpisodeId, lastCmdId;
  uint8_t kind, arg, theme, screen, brightnessPct, autoBright, sleeping, alarmArmed, alarmAcked, dwellSec;
  const char *goodJson="{\"ok\":true,\"data\":{\"ts\":42,\"score\":140,\"alarmActive\":true,\"dataStale\":false,\"stateRoll\":{\"up\":1,\"degraded\":0,\"down\":0,\"unknown\":0,\"maint\":0},\"alerts\":{\"info\":0,\"warn\":0,\"crit\":1},\"meanLoadPct\":12,\"rxKbps\":340,\"txKbps\":70000,\"rttTenthMs\":300,\"lossPermille\":500,\"pools\":[],\"systems\":[],\"topAlert\":null}}";
  const char *badJson="{\"ok\":true,\"data\":{\"ts\":43,\"score\":1,\"stateRoll\":{},\"alerts\":{},\"pools\":[null],\"systems\":[]}}";

  frameLen=snapshotFrame(7u,frame); panelParserInit(&parser); frames=0;
  for(i=0;i<frameLen;++i) panelParserFeed(&parser,frame+i,1u,(uint32_t)i,receive,NULL);
  if(require(frames==1,"round trip frame missing")||require(received.seq==7u&&received.score==140u&&!strcmp(received.pools[0].name,"CORE"),"round trip mismatch")) return 1;

  memcpy(stream,frame,frameLen); stream[8]^=1u; panelParserFeed(&parser,stream,frameLen,1000u,receive,NULL);
  if(require(frames==1&&parser.crcErrors==1u,"CRC corruption accepted")) return 1;

  panelParserInit(&parser); frames=0; stream[0]=PANEL_MAGIC0;stream[1]=PANEL_MAGIC1;stream[2]=PANEL_PROTO_VERSION;stream[3]=PANEL_FT_SNAPSHOT;stream[4]=40u;stream[5]=0u;offset=6u;
  for(i=0;i<4u;++i){frameLen=snapshotFrame((uint16_t)(10u+i),frame);memcpy(stream+offset,frame,frameLen);offset+=frameLen;}
  panelParserFeed(&parser,stream,offset,2000u,receive,NULL);
  if(require(frames==4&&parser.crcErrors==1u,"resync drain did not dispatch four frames")) return 1;

  panelParserInit(&parser); frames=0; frameLen=snapshotFrame(20u,frame); stream[0]=PANEL_MAGIC0;memcpy(stream+1u,frame,frameLen);panelParserFeed(&parser,stream,frameLen+1u,3000u,receive,NULL);
  if(require(frames==1,"overlapping magic was not recovered")) return 1;

  if(require(panelSeqNewer(1u,0u)&&panelSeqNewer(0u,65535u)&&!panelSeqNewer(0u,0u)&&!panelSeqNewer(65535u,0u)&&!panelSeqNewer(0x8000u,0u),"RFC1982 sequence comparison failed")) return 1;

  if(require(panelEncodeControl(0x12345678u,PANEL_CTL_BRIGHTNESS,99u,control,sizeof(control))==PANEL_CONTROL_SIZE,"CONTROL encode failed")||require(control[0]==0x78u&&control[3]==0x12u&&control[4]==PANEL_CTL_BRIGHTNESS&&control[5]==99u&&control[6]==0u&&control[7]==0u,"CONTROL offsets incorrect")||require(panelDecodeControl(control,sizeof(control),&cmdId,&kind,&arg)==0&&cmdId==0x12345678u&&kind==PANEL_CTL_BRIGHTNESS&&arg==99u,"CONTROL round trip failed")||require(panelEncodeControl(1u,1u,1u,control,PANEL_CONTROL_SIZE-1u)==0u,"CONTROL short encode accepted")||require(panelDecodeControl(control,PANEL_CONTROL_SIZE-1u,&cmdId,&kind,&arg)==-1,"CONTROL short decode accepted")) return 1;
  controlFrames=0; panelParserInit(&parser); frameLen=panelEncodeFrame(PANEL_FT_CONTROL,control,sizeof(control),frame,sizeof(frame));
  for(i=0;i<frameLen;++i) panelParserFeed(&parser,frame+i,1u,(uint32_t)i,receive,NULL);
  if(require(controlFrames==1,"CONTROL parser dispatch missing")) return 1;

  if(require(panelEncodeState(3u,2u,100u,1u,0u,1u,1u,30u,0x10203040u,0x50607080u,state,sizeof(state))==PANEL_STATE_SIZE,"STATE encode failed")||require(state[0]==3u&&state[6]==1u&&state[7]==30u&&state[8]==0x40u&&state[11]==0x10u&&state[12]==0x80u&&state[15]==0x50u,"STATE offsets incorrect")||require(panelDecodeState(state,sizeof(state),&theme,&screen,&brightnessPct,&autoBright,&sleeping,&alarmArmed,&alarmAcked,&dwellSec,&ackedEpisodeId,&lastCmdId)==0&&theme==3u&&screen==2u&&brightnessPct==100u&&autoBright==1u&&!sleeping&&alarmArmed&&alarmAcked&&dwellSec==30u&&ackedEpisodeId==0x10203040u&&lastCmdId==0x50607080u,"STATE round trip failed")||require(panelEncodeState(0u,0u,0u,0u,0u,0u,0u,0u,0u,0u,state,PANEL_STATE_SIZE-1u)==0u,"STATE short encode accepted")||require(panelDecodeState(state,PANEL_STATE_SIZE-1u,&theme,&screen,&brightnessPct,&autoBright,&sleeping,&alarmArmed,&alarmAcked,&dwellSec,&ackedEpisodeId,&lastCmdId)==-1,"STATE short decode accepted")) return 1;
  stateFrames=0; panelParserInit(&parser); frameLen=panelEncodeFrame(PANEL_FT_STATE,state,sizeof(state),frame,sizeof(frame));
  for(i=0;i<frameLen;++i) panelParserFeed(&parser,frame+i,1u,(uint32_t)i,receive,NULL);
  if(require(stateFrames==1,"STATE parser dispatch missing")) return 1;
  memset(stream,0,sizeof(stream)); memcpy(stream,control,sizeof(control));
  if(require(panelDecodeControl(stream,PANEL_CONTROL_SIZE+1u,&cmdId,&kind,&arg)==0,"CONTROL trailing bytes rejected")) return 1;
  memset(stream,0,sizeof(stream)); memcpy(stream,state,sizeof(state));
  if(require(panelDecodeState(stream,PANEL_STATE_SIZE+1u,&theme,&screen,&brightnessPct,&autoBright,&sleeping,&alarmArmed,&alarmAcked,&dwellSec,&ackedEpisodeId,&lastCmdId)==0,"STATE trailing bytes rejected")) return 1;

  memset(&lastGood,0x5a,sizeof(lastGood));
  if(require(panelParseSnapshotForTest(goodJson,&lastGood)==0&&lastGood.rxKbps==340u&&lastGood.txKbps==70000u&&lastGood.rttTenthMs==300u&&lastGood.lossPermille==500u,"valid snapshot parse failed")) return 1;
  if(require(panelParseSnapshotForTest(badJson,&lastGood)==-1&&lastGood.ts==42u&&lastGood.score==140u,"malformed snapshot clobbered last good state")) return 1;

  /* forwardCommands must navigate the real API envelope: commands lives at
   * data.commands, not the root. This is the exact bug class that shipped —
   * a root-level lookup silently forwards nothing. Feed a pipe, then parse
   * what came out and assert a CONTROL frame with the right cmdId emerged. */
  {
    int pipeFds[2];
    uint8_t buf[256]; ssize_t got;
    const char *envelopeJson="{\"ok\":true,\"data\":{\"ts\":1,\"commands\":[{\"id\":21,\"kind\":1,\"arg\":0}]}}";
    const char *rootJson="{\"ok\":true,\"commands\":[{\"id\":22,\"kind\":1,\"arg\":0}]}";
    if(require(pipe(pipeFds)==0,"pipe failed")) return 1;
    /* gate closed: nothing may be written even for a well-formed envelope */
    if(require(panelForwardCommandsForTest(pipeFds[1],envelopeJson,0)==0,"forward with gate closed errored")) return 1;
    /* gate open, real envelope: exactly one CONTROL frame with cmdId 21 */
    if(require(panelForwardCommandsForTest(pipeFds[1],envelopeJson,1)==0,"forward with envelope errored")) return 1;
    /* root-level commands (the buggy shape) must forward nothing */
    if(require(panelForwardCommandsForTest(pipeFds[1],rootJson,1)==0,"forward with root json errored")) return 1;
    close(pipeFds[1]);
    controlFrames=0; cmdId=0u; panelParserInit(&parser);
    while((got=read(pipeFds[0],buf,sizeof(buf)))>0)
      for(i=0;i<(size_t)got;++i) panelParserFeed(&parser,buf+i,1u,(uint32_t)i,receiveControl,&cmdId);
    close(pipeFds[0]);
    if(require(controlFrames==1,"forwardCommands did not emit exactly one CONTROL frame (envelope navigation broken?)")) return 1;
    if(require(cmdId==21u,"forwarded CONTROL carried wrong cmdId")) return 1;
  }
  puts("codec tests passed");
  return 0;
}
