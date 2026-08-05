/* solariPanel: authenticated API poller and Galactic Unicorn serial bridge. */
#define _POSIX_C_SOURCE 200809L
#include "../protocol.h"
#include "cJSON.h"
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_CONFIG "/etc/solari-panel/solari-panel.conf"
#define CA_FILE "/etc/solari-panel/ca.pem"
#define BODY_MAX 65536u
typedef struct { char apiBase[512], user[128], passFile[512], serialDev[512]; unsigned int pollSec, snapshotSec; } Config;
typedef struct { char data[BODY_MAX + 1u]; size_t len; long status; char contentType[128]; } HttpResponse;
typedef struct { uint8_t theme, screen, brightnessPct, autoBright, sleeping, alarmArmed, alarmAcked, dwellSec; uint32_t ackedEpisodeId, lastCmdId; } PanelStateReport;
typedef struct { CURL *curl; const Config *config; PanelStateReport lastState; uint32_t lastStatePostMs; int stateSeen, haveState, withholdingLogged; } PanelLink;
static volatile sig_atomic_t running = 1;

/* Purpose: write timestamped operational log records. Input: printf-style message. Output: one stdout line. */
static void logMessage(const char *format, ...) { va_list ap; time_t t=time(NULL); struct tm tmValue; char stamp[32]; if(gmtime_r(&t,&tmValue)!=NULL)strftime(stamp,sizeof(stamp),"%Y-%m-%dT%H:%M:%SZ",&tmValue);else strcpy(stamp,"time-unknown"); printf("%s solariPanel: ",stamp);va_start(ap,format);vprintf(format,ap);va_end(ap);putchar('\n');fflush(stdout); }
/* Purpose: return monotonic milliseconds. Input: none. Output: wrapping millisecond tick. */
static uint32_t nowMs(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (uint32_t)((uint64_t)ts.tv_sec*1000u+(uint64_t)ts.tv_nsec/1000000u); }
/* Purpose: terminate loop on service signals. Input: signal number. Output: sets process stop flag. */
static void stopSignal(int signalNumber) { (void)signalNumber; running=0; }
/* Purpose: trim mutable config text. Input: string. Output: pointer to non-space start and NUL end. */
static char *trim(char *value) { char *end; while(*value==' '||*value=='\t'||*value=='\r'||*value=='\n')++value; end=value+strlen(value);while(end>value&&(end[-1]==' '||end[-1]=='\t'||end[-1]=='\r'||end[-1]=='\n'))*--end='\0';return value; }
/* Purpose: load key=value daemon configuration. Input: path/config destination. Output: zero success, -1 invalid/missing. */
static int loadConfig(const char *path, Config *config) { FILE *file;char line[1200];memset(config,0,sizeof(*config));config->pollSec=5u;config->snapshotSec=2u;strcpy(config->serialDev,"/dev/serial/by-id/*");file=fopen(path,"r");if(file==NULL){logMessage("cannot read config %s: %s",path,strerror(errno));return -1;}while(fgets(line,sizeof(line),file)!=NULL){char *key,*value,*equals;if(line[0]=='#'||line[0]=='\n')continue;equals=strchr(line,'=');if(equals==NULL)continue;*equals='\0';key=trim(line);value=trim(equals+1);if(strcmp(key,"apiBase")==0)snprintf(config->apiBase,sizeof(config->apiBase),"%s",value);else if(strcmp(key,"user")==0)snprintf(config->user,sizeof(config->user),"%s",value);else if(strcmp(key,"passFile")==0)snprintf(config->passFile,sizeof(config->passFile),"%s",value);else if(strcmp(key,"serialDev")==0)snprintf(config->serialDev,sizeof(config->serialDev),"%s",value);else if(strcmp(key,"pollSec")==0)config->pollSec=(unsigned int)strtoul(value,NULL,10);else if(strcmp(key,"snapshotSec")==0)config->snapshotSec=(unsigned int)strtoul(value,NULL,10);}fclose(file);if(config->apiBase[0]=='\0'||config->user[0]=='\0'||config->passFile[0]=='\0'||config->pollSec==0u||config->snapshotSec==0u){logMessage("config requires apiBase, user, passFile and positive intervals");return -1;}return 0; }
/* Purpose: read password without exposing it to argv or environment. Input: 0600-file path/buffer. Output: zero success, -1 failure. */
static int readPassword(const char *path,char *out,size_t cap){FILE *file=fopen(path,"r");struct stat status;char *value;if(file==NULL){logMessage("password file %s: cannot open (%s)",path,strerror(errno));return -1;}if(fstat(fileno(file),&status)!=0){logMessage("password file %s: fstat failed (%s)",path,strerror(errno));fclose(file);return -1;}if((status.st_mode&(S_IRWXG|S_IRWXO))!=0){logMessage("password file %s: refusing group/other-accessible mode %04o — chmod 600 it",path,(unsigned)(status.st_mode&07777));fclose(file);return -1;}if(fgets(out,(int)cap,file)==NULL){logMessage("password file %s: empty or unreadable",path);fclose(file);return -1;}fclose(file);value=trim(out);memmove(out,value,strlen(value)+1u);if(out[0]=='\0'){logMessage("password file %s: blank after trim",path);return -1;}return 0;}
/* Purpose: capture bounded HTTP body bytes. Input: curl bytes/response. Output: byte count consumed. */
static size_t bodyWrite(char *data,size_t size,size_t count,void *user){HttpResponse *response=user;size_t bytes=size*count,room=BODY_MAX-response->len;if(bytes>room)return 0u;memcpy(response->data+response->len,data,bytes);response->len+=bytes;response->data[response->len]='\0';return bytes;}
/* Purpose: capture HTTP content type. Input: curl header line/response. Output: line byte count. */
static size_t headerWrite(char *data,size_t size,size_t count,void *user){HttpResponse *response=user;size_t bytes=size*count;static const char prefix[]="Content-Type:";if(bytes>=sizeof(prefix)-1u&&strncasecmp(data,prefix,sizeof(prefix)-1u)==0){size_t n=bytes-(sizeof(prefix)-1u);if(n>=sizeof(response->contentType))n=sizeof(response->contentType)-1u;memcpy(response->contentType,data+sizeof(prefix)-1u,n);response->contentType[n]='\0';}return bytes;}
/* Purpose: configure common hardened curl settings. Input: handle/response. Output: configured handle. */
static void setCurlCommon(CURL *curl,HttpResponse *response){memset(response,0,sizeof(*response));curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,bodyWrite);curl_easy_setopt(curl,CURLOPT_WRITEDATA,response);curl_easy_setopt(curl,CURLOPT_HEADERFUNCTION,headerWrite);curl_easy_setopt(curl,CURLOPT_HEADERDATA,response);curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,5L);curl_easy_setopt(curl,CURLOPT_TIMEOUT,10L);curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,0L);curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,1L);curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,2L);curl_easy_setopt(curl,CURLOPT_CAINFO,CA_FILE);
  /* Fresh connection per request: the 5 s poll races Apache's 5 s keepalive
   * timeout, yielding sporadic CURLE_SEND_ERROR on a just-closed reuse.
   * At 0.2 req/s a new TLS handshake per poll is negligible.               */
  curl_easy_setopt(curl,CURLOPT_FORBID_REUSE,1L);curl_easy_setopt(curl,CURLOPT_FRESH_CONNECT,1L);}
/* Purpose: issue an HTTP request and collect response metadata. Input: curl/url/post body/response. Output: CURLcode. */
static CURLcode request(CURL *curl,const char *url,const char *post,HttpResponse *response){CURLcode result;setCurlCommon(curl,response);curl_easy_setopt(curl,CURLOPT_URL,url);curl_easy_setopt(curl,CURLOPT_POST,post!=NULL?1L:0L);if(post!=NULL){curl_easy_setopt(curl,CURLOPT_POSTFIELDS,post);curl_easy_setopt(curl,CURLOPT_POSTFIELDSIZE,(long)strlen(post));}result=curl_easy_perform(curl);if(result==CURLE_OK)curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&response->status);return result;}
/* Purpose: create an authenticated in-memory-cookie curl client. Input: config/password. Output: ready handle or NULL. */
static CURL *login(const Config *config,const char *password){CURL *curl=curl_easy_init();HttpResponse response;cJSON *json;char url[768];char *body;struct curl_slist *headers=NULL;CURLcode result;if(curl==NULL)return NULL;json=cJSON_CreateObject();if(json==NULL||!cJSON_AddStringToObject(json,"username",config->user)||!cJSON_AddStringToObject(json,"password",password)){cJSON_Delete(json);curl_easy_cleanup(curl);return NULL;}body=cJSON_PrintUnformatted(json);cJSON_Delete(json);if(body==NULL){curl_easy_cleanup(curl);return NULL;}curl_easy_setopt(curl,CURLOPT_COOKIEFILE,"");snprintf(url,sizeof(url),"%s/api/auth/login",config->apiBase);headers=curl_slist_append(NULL,"Content-Type: application/json");if(headers==NULL){cJSON_free(body);curl_easy_cleanup(curl);return NULL;}curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);curl_easy_setopt(curl,CURLOPT_COPYPOSTFIELDS,body);result=request(curl,url,body,&response);curl_easy_setopt(curl,CURLOPT_HTTPHEADER,NULL);curl_slist_free_all(headers);memset(body,0,strlen(body));cJSON_free(body);if(result!=CURLE_OK||response.status<200L||response.status>=300L){logMessage("login failed (HTTP %ld)",response.status);curl_easy_cleanup(curl);return NULL;}return curl;}
/* Purpose: upload the latest decoded STATE using the hardened shared curl handle. Input: link/state. Output: zero on an accepted JSON response, -1 otherwise. */
static int postState(PanelLink *link,const PanelStateReport *state){cJSON *json;char *body,url[768];struct curl_slist *headers;HttpResponse response;CURLcode result;if(link->curl==NULL)return -1;json=cJSON_CreateObject();if(json==NULL)return -1;if(!cJSON_AddNumberToObject(json,"theme",state->theme)||!cJSON_AddNumberToObject(json,"screen",state->screen)||!cJSON_AddNumberToObject(json,"brightnessPct",state->brightnessPct)||!cJSON_AddNumberToObject(json,"autoBright",state->autoBright)||!cJSON_AddNumberToObject(json,"sleeping",state->sleeping)||!cJSON_AddNumberToObject(json,"alarmArmed",state->alarmArmed)||!cJSON_AddNumberToObject(json,"alarmAcked",state->alarmAcked)||!cJSON_AddNumberToObject(json,"dwellSec",state->dwellSec)||!cJSON_AddNumberToObject(json,"ackedEpisodeId",state->ackedEpisodeId)||!cJSON_AddNumberToObject(json,"lastCmdId",state->lastCmdId)){cJSON_Delete(json);return -1;}body=cJSON_PrintUnformatted(json);cJSON_Delete(json);if(body==NULL)return -1;headers=curl_slist_append(NULL,"Content-Type: application/json");if(headers==NULL){cJSON_free(body);return -1;}snprintf(url,sizeof(url),"%s/api/panel/state",link->config->apiBase);curl_easy_setopt(link->curl,CURLOPT_HTTPHEADER,headers);result=request(link->curl,url,body,&response);curl_easy_setopt(link->curl,CURLOPT_HTTPHEADER,NULL);curl_slist_free_all(headers);cJSON_free(body);if(result!=CURLE_OK||response.status<200L||response.status>=300L||strncasecmp(trim(response.contentType),"application/json",16u)!=0){logMessage("panel state post failed (curl=%d HTTP=%ld)",(int)result,response.status);return -1;}return 0;}
/* Purpose: clamp signed JSON integer to protocol byte range. Input: JSON item/default. Output: 0..255 value. */
static uint8_t byteValue(const cJSON *item,int fallback){int n=cJSON_IsNumber(item)?item->valueint:fallback;return (uint8_t)(n<0?0:(n>255?255:n));}
/* Purpose: clamp a JSON number to an unsigned 16-bit wire value. Input: item/default. Output: 0..65535. */
static uint16_t u16Value(const cJSON *item,uint16_t fallback){double n=cJSON_IsNumber(item)?item->valuedouble:(double)fallback;return (uint16_t)(n<0.0?0.0:(n>65535.0?65535.0:n));}
/* Purpose: clamp a JSON number to an unsigned 32-bit wire value. Input: item/default. Output: 0..UINT32_MAX. */
static uint32_t u32Value(const cJSON *item,uint32_t fallback){double n=cJSON_IsNumber(item)?item->valuedouble:(double)fallback;return (uint32_t)(n<0.0?0.0:(n>4294967295.0?4294967295.0:n));}
/* Purpose: map API state labels to protocol values. Input: state string. Output: PanelState value. */
static uint8_t stateValue(const cJSON *item){const char *s=cJSON_GetStringValue((cJSON *)item);if(s==NULL)return PANEL_ST_UNKNOWN;if(strcmp(s,"up")==0||strcmp(s,"ok")==0)return PANEL_ST_OK;if(strcmp(s,"degraded")==0)return PANEL_ST_DEGRADED;if(strcmp(s,"down")==0)return PANEL_ST_DOWN;if(strcmp(s,"maint")==0)return PANEL_ST_MAINT;return PANEL_ST_UNKNOWN;}
/* Purpose: map API alert severity labels to protocol values. Input: severity string item. Output: PanelSeverity value. */
static uint8_t severityValue(const cJSON *item){const char *s=cJSON_GetStringValue((cJSON *)item);if(s!=NULL&&strcmp(s,"crit")==0)return PANEL_SEV_CRIT;if(s!=NULL&&strcmp(s,"warn")==0)return PANEL_SEV_WARN;return PANEL_SEV_INFO;}
/* Purpose: validate and transform one complete panel JSON document. Input: JSON bytes/snapshot. Output: zero valid, -1 otherwise. */
static int parseSnapshot(const char *text,PanelSnapshot *snapshot) {
  PanelSnapshot staged;
  cJSON *root=cJSON_Parse(text),*data,*pools,*systems,*roll,*alerts,*item,*ts,*score,*top;
  int i,count;
  if(root==NULL || snapshot==NULL)return -1;
  data=cJSON_GetObjectItemCaseSensitive(root,"data");
  if(!cJSON_IsObject(root)||!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root,"ok"))||!cJSON_IsObject(data)){cJSON_Delete(root);return -1;}
  ts=cJSON_GetObjectItemCaseSensitive(data,"ts"); score=cJSON_GetObjectItemCaseSensitive(data,"score");
  roll=cJSON_GetObjectItemCaseSensitive(data,"stateRoll"); alerts=cJSON_GetObjectItemCaseSensitive(data,"alerts");
  pools=cJSON_GetObjectItemCaseSensitive(data,"pools"); systems=cJSON_GetObjectItemCaseSensitive(data,"systems");
  if(!cJSON_IsNumber(ts)||!cJSON_IsNumber(score)||!cJSON_IsObject(roll)||!cJSON_IsObject(alerts)||!cJSON_IsArray(pools)||!cJSON_IsArray(systems)||(count=cJSON_GetArraySize(pools))>(int)PANEL_MAX_POOLS||cJSON_GetArraySize(systems)>(int)PANEL_MAX_SYSTEMS){cJSON_Delete(root);return -1;}
  memset(&staged,0,sizeof(staged)); staged.ts=u32Value(ts,0u); staged.score=u16Value(score,0u);
  /* Roster sizes drive both parse loops and the wire encoder; count was
   * validated against PANEL_MAX_POOLS / PANEL_MAX_SYSTEMS above. */
  staged.poolCount=(uint8_t)count; staged.systemCount=(uint8_t)cJSON_GetArraySize(systems);
  staged.alarmActive=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data,"alarmActive"))?1u:0u;
  staged.dataStale=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data,"dataStale"))?1u:0u;
  staged.stateRoll[0]=byteValue(cJSON_GetObjectItemCaseSensitive(roll,"up"),0); staged.stateRoll[1]=byteValue(cJSON_GetObjectItemCaseSensitive(roll,"degraded"),0); staged.stateRoll[2]=byteValue(cJSON_GetObjectItemCaseSensitive(roll,"down"),0); staged.stateRoll[3]=byteValue(cJSON_GetObjectItemCaseSensitive(roll,"unknown"),0); staged.stateRoll[4]=byteValue(cJSON_GetObjectItemCaseSensitive(roll,"maint"),0);
  staged.alertCounts[0]=byteValue(cJSON_GetObjectItemCaseSensitive(alerts,"info"),0); staged.alertCounts[1]=byteValue(cJSON_GetObjectItemCaseSensitive(alerts,"warn"),0); staged.alertCounts[2]=byteValue(cJSON_GetObjectItemCaseSensitive(alerts,"crit"),0);
  staged.meanLoadPct=byteValue(cJSON_GetObjectItemCaseSensitive(data,"meanLoadPct"),0); staged.rxKbps=u32Value(cJSON_GetObjectItemCaseSensitive(data,"rxKbps"),0u); staged.txKbps=u32Value(cJSON_GetObjectItemCaseSensitive(data,"txKbps"),0u); staged.rttTenthMs=u16Value(cJSON_GetObjectItemCaseSensitive(data,"rttTenthMs"),0u); staged.lossPermille=u16Value(cJSON_GetObjectItemCaseSensitive(data,"lossPermille"),0u);
  for(i=0;i<count;++i){item=cJSON_GetArrayItem(pools,i);if(!cJSON_IsObject(item)){cJSON_Delete(root);return -1;}snprintf(staged.pools[i].name,sizeof(staged.pools[i].name),"%s",cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item,"name"))?cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item,"name")):"");staged.pools[i].tier=byteValue(cJSON_GetObjectItemCaseSensitive(item,"tier"),0);staged.pools[i].ok=byteValue(cJSON_GetObjectItemCaseSensitive(item,"up"),0);staged.pools[i].degraded=byteValue(cJSON_GetObjectItemCaseSensitive(item,"degraded"),0);staged.pools[i].down=byteValue(cJSON_GetObjectItemCaseSensitive(item,"down"),0);staged.pools[i].unknown=byteValue(cJSON_GetObjectItemCaseSensitive(item,"unknown"),0);staged.pools[i].maint=byteValue(cJSON_GetObjectItemCaseSensitive(item,"maint"),0);staged.pools[i].total=byteValue(cJSON_GetObjectItemCaseSensitive(item,"total"),0);staged.pools[i].loadPct=byteValue(cJSON_GetObjectItemCaseSensitive(item,"loadPct"),0);}
  for(i=0;i<(int)staged.systemCount;++i){item=cJSON_GetArrayItem(systems,i);if(!cJSON_IsObject(item)){cJSON_Delete(root);return -1;}snprintf(staged.systems[i].name,sizeof(staged.systems[i].name),"%s",cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item,"name"))?cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item,"name")):"");staged.systems[i].state=stateValue(cJSON_GetObjectItemCaseSensitive(item,"state"));staged.systems[i].pool=byteValue(cJSON_GetObjectItemCaseSensitive(item,"pool"),255);staged.systems[i].tier=byteValue(cJSON_GetObjectItemCaseSensitive(item,"tier"),0);staged.systems[i].loadPct=byteValue(cJSON_GetObjectItemCaseSensitive(item,"loadPct"),0);}
  top=cJSON_GetObjectItemCaseSensitive(data,"topAlert");if(cJSON_IsObject(top)){staged.hasTopAlert=1u;staged.topAlert.alertId=u32Value(cJSON_GetObjectItemCaseSensitive(top,"id"),0u);staged.topAlert.episodeId=u32Value(cJSON_GetObjectItemCaseSensitive(data,"episodeId"),0u);staged.topAlert.severity=severityValue(cJSON_GetObjectItemCaseSensitive(top,"severity"));snprintf(staged.topAlert.subject,sizeof(staged.topAlert.subject),"%s",cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(top,"subject"))?cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(top,"subject")):"");snprintf(staged.topAlert.detail,sizeof(staged.topAlert.detail),"%s",cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(top,"detail"))?cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(top,"detail")):"");}
  cJSON_Delete(root);
  /* seq belongs to the sender's SESSION, not the payload: preserve the
   * caller's counter across the staged copy, or every poll resets it and
   * the receiver (correctly) discards all frames as duplicates of seq 1. */
  staged.seq=snapshot->seq;
  *snapshot=staged;return 0;
}

/* Purpose: open and configure a raw serial CDC device. Input: configured glob or literal. Output: nonblocking fd, or -1. */
static int openSerial(const char *pattern) { glob_t matches;size_t i,count=1u;const char *paths[1];int fd=-1;struct termios settings;memset(&matches,0,sizeof(matches));if(strpbrk(pattern,"*?[")!=NULL){if(glob(pattern,0,NULL,&matches)!=0||matches.gl_pathc==0u){globfree(&matches);return -1;}count=matches.gl_pathc;for(i=0u;i<count;++i){fd=open(matches.gl_pathv[i],O_RDWR|O_NOCTTY|O_NONBLOCK);if(fd<0)continue;if(tcgetattr(fd,&settings)==0){logMessage("serial connected: %s",matches.gl_pathv[i]);break;}close(fd);fd=-1;}globfree(&matches);}else{paths[0]=pattern;fd=open(paths[0],O_RDWR|O_NOCTTY|O_NONBLOCK);if(fd>=0&&tcgetattr(fd,&settings)!=0){close(fd);fd=-1;}if(fd>=0)logMessage("serial connected: %s",paths[0]);}if(fd<0)return -1;settings.c_iflag&=(tcflag_t)~(IGNBRK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL|IXON);settings.c_oflag&=(tcflag_t)~OPOST;settings.c_lflag&=(tcflag_t)~(ECHO|ECHONL|ICANON|ISIG|IEXTEN);settings.c_cflag&=(tcflag_t)~(CSIZE|PARENB);settings.c_cflag|=CS8;settings.c_cc[VMIN]=0;settings.c_cc[VTIME]=0;cfsetispeed(&settings,B115200);cfsetospeed(&settings,B115200);if(tcsetattr(fd,TCSANOW,&settings)!=0){close(fd);return -1;}return fd;}
/* Purpose: sleep briefly without using obsolete interfaces. Input: milliseconds. Output: elapsed sleep or interruption. */
static void sleepMs(long milliseconds) { struct timespec pauseTime;pauseTime.tv_sec=milliseconds/1000L;pauseTime.tv_nsec=(milliseconds%1000L)*1000000L;nanosleep(&pauseTime,NULL); }
/* Purpose: reliably write one complete frame to serial. Input: fd/frame bytes. Output: zero success, -1 timeout/disconnect. */
static int writeAll(int fd,const uint8_t *bytes,size_t length){size_t sent=0u;uint32_t deadline=nowMs()+2000u;while(sent<length){ssize_t result=write(fd,bytes+sent,length-sent);if(result>0){sent+=(size_t)result;continue;}if(result<0&&errno==EINTR)continue;if(result<0&&(errno==EAGAIN||errno==EWOULDBLOCK)){if((int32_t)(nowMs()-deadline)>=0)return -1;sleepMs(10L);continue;}return -1;}return 0;}
/* Purpose: upload and record one decoded STATE report. Input: parser callback values/link context. Output: state capability and upload cadence maintained. */
static void handleState(const uint8_t *payload,size_t length,PanelLink *link){PanelStateReport state;int changed;memset(&state,0,sizeof(state));if(panelDecodeState(payload,length,&state.theme,&state.screen,&state.brightnessPct,&state.autoBright,&state.sleeping,&state.alarmArmed,&state.alarmAcked,&state.dwellSec,&state.ackedEpisodeId,&state.lastCmdId)!=0){logMessage("malformed panel STATE (%lu bytes)",(unsigned long)length);return;}link->stateSeen=1;changed=!link->haveState||memcmp(&state,&link->lastState,sizeof(state))!=0;if(changed)logMessage("panel STATE theme=%u screen=%u brightness=%u auto=%u sleeping=%u armed=%u acked=%u dwell=%u episode=%lu lastCmdId=%lu",state.theme,state.screen,state.brightnessPct,state.autoBright,state.sleeping,state.alarmArmed,state.alarmAcked,state.dwellSec,(unsigned long)state.ackedEpisodeId,(unsigned long)state.lastCmdId);if(changed||(uint32_t)(nowMs()-link->lastStatePostMs)>=30000u){if(postState(link,&state)==0){link->lastState=state;link->haveState=1;link->lastStatePostMs=nowMs();}}}
/* Purpose: log diagnostic panel frames and dispatch STATE reports. Input: parsed frame/link context. Output: one safe diagnostic action. */
static void panelFrame(uint8_t type,const uint8_t *payload,size_t length,void *user){size_t i,n;char text[160];PanelLink *link=(PanelLink *)user;if(type==PANEL_FT_HELLO)logMessage("panel HELLO (%lu bytes)",(unsigned long)length);else if(type==PANEL_FT_EVENT&&length>=2u)logMessage("panel EVENT kind=%u arg=%u",payload[0],payload[1]);else if(type==PANEL_FT_LOG){n=length<sizeof(text)-1u?length:sizeof(text)-1u;for(i=0;i<n;++i)text[i]=(payload[i]>=32u&&payload[i]<127u)?(char)payload[i]:'.';text[n]='\0';logMessage("panel LOG %s",text);}else if(type==PANEL_FT_STATE&&link!=NULL)handleState(payload,length,link);}
/* Purpose: drain incoming serial diagnostics. Input: fd/parser. Output: parser receives all currently available bytes. */
static int readSerial(int fd,PanelParser *parser,PanelLink *link){uint8_t bytes[256];ssize_t n;do{n=read(fd,bytes,sizeof(bytes));if(n>0)panelParserFeed(parser,bytes,(size_t)n,nowMs(),panelFrame,link);}while(n>0);return n<0&&(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)?-1:0;}
/* Purpose: send a snapshot or keepalive frame. Input: fd/current snapshot/selection. Output: zero success, -1 failure. */
static int sendFrame(int fd,const PanelSnapshot *snapshot,int sendSnapshot){uint8_t payload[PANEL_MAX_PAYLOAD],frame[PANEL_HDR_SIZE+PANEL_MAX_PAYLOAD+PANEL_CRC_SIZE];size_t payloadLength=0u,frameLength;if(sendSnapshot)payloadLength=panelEncodeSnapshot(snapshot,payload,sizeof(payload));frameLength=panelEncodeFrame(sendSnapshot?PANEL_FT_SNAPSHOT:PANEL_FT_PING,sendSnapshot?payload:NULL,payloadLength,frame,sizeof(frame));return frameLength==0u?-1:writeAll(fd,frame,frameLength);}
/* Purpose: forward every currently served command in API order after a snapshot write. Input: fd/poll JSON/link state. Output: zero success, -1 on serial failure. */
static int forwardCommands(int fd,const char *text,PanelLink *link){cJSON *root,*commands,*item,*idItem,*kindItem,*argItem;int i,count;uint8_t payload[PANEL_CONTROL_SIZE],frame[PANEL_HDR_SIZE+PANEL_CONTROL_SIZE+PANEL_CRC_SIZE];size_t frameLength;uint32_t cmdId;int kind,arg;root=cJSON_Parse(text);if(root==NULL){logMessage("panel commands malformed");return 0;}/* The API envelope is {"ok":..,"data":{..,"commands":[..]}} — commands
 * lives under "data" (same navigation as parseSnapshot), not at the root. */
{cJSON *data=cJSON_GetObjectItemCaseSensitive(root,"data");commands=data!=NULL?cJSON_GetObjectItemCaseSensitive(data,"commands"):NULL;}if(commands==NULL){cJSON_Delete(root);return 0;}if(!cJSON_IsArray(commands)||(count=cJSON_GetArraySize(commands))>16){cJSON_Delete(root);logMessage("panel commands malformed or over limit");return 0;}if(count!=0&&!link->stateSeen){if(!link->withholdingLogged){logMessage("withholding panel commands until STATE confirms CONTROL capability");link->withholdingLogged=1;}cJSON_Delete(root);return 0;}for(i=0;i<count;++i){item=cJSON_GetArrayItem(commands,i);idItem=cJSON_IsObject(item)?cJSON_GetObjectItemCaseSensitive(item,"id"):NULL;kindItem=cJSON_IsObject(item)?cJSON_GetObjectItemCaseSensitive(item,"kind"):NULL;argItem=cJSON_IsObject(item)?cJSON_GetObjectItemCaseSensitive(item,"arg"):NULL;cmdId=u32Value(idItem,0u);kind=cJSON_IsNumber(kindItem)?kindItem->valueint:-1;arg=cJSON_IsNumber(argItem)?argItem->valueint:-1;/* R3: NO kind-range check here — the firmware consumes unknown kinds (a
 * consuming reject), so forwarding under version skew yields an honest
 * outcome; a daemon-side range check would strand the command pending and
 * let the late-confirm sweep falsify it as applied. Structural guards only. */
if(!cJSON_IsNumber(idItem)||!cJSON_IsNumber(kindItem)||!cJSON_IsNumber(argItem)||cmdId==0u||kind<0||kind>255||arg<0||arg>255||panelEncodeControl(cmdId,(uint8_t)kind,(uint8_t)arg,payload,sizeof(payload))!=PANEL_CONTROL_SIZE){logMessage("skipping invalid command id=%u kind=%u",cmdId,(unsigned int)(kind<0?0:kind));continue;}frameLength=panelEncodeFrame(PANEL_FT_CONTROL,payload,sizeof(payload),frame,sizeof(frame));if(frameLength==0u||writeAll(fd,frame,frameLength)!=0){cJSON_Delete(root);return -1;}logMessage("forwarded panel command id=%lu kind=%d arg=%d",(unsigned long)cmdId,kind,arg);}cJSON_Delete(root);return 0;}
/* glanceWrite — publish at-a-glance facts for the solari-glance App Lab app,
 * which mirrors panel health onto the UNO Q's on-board 8x13 LED matrix.
 * Input:  latest snapshot (may be absent), last poll outcome, link, serial fd
 *         state. Output: atomic rewrite of /run/solari-panel/glance.json via
 *         rename so the reader never sees a torn file. Every failure here is
 *         silent by design — the glance is cosmetic and must never disturb
 *         the daemon's real work. */
static void glanceWrite(const PanelSnapshot *snap,int haveSnap,int pollOk,const PanelLink *link,int serialUp){
  FILE *f=fopen("/run/solari-panel/glance.json.tmp","w");
  if(f==NULL)return;
  fprintf(f,"{\"ts\":%lu,\"pollOk\":%d,\"serialUp\":%d,\"stateSeen\":%d",
          (unsigned long)time(NULL),pollOk?1:0,serialUp?1:0,link->stateSeen?1:0);
  if(haveSnap)
    fprintf(f,",\"score\":%u,\"alarmActive\":%u,\"dataStale\":%u,"
              "\"up\":%u,\"degraded\":%u,\"down\":%u,\"unknown\":%u,\"maint\":%u,"
              "\"warn\":%u,\"crit\":%u",
            snap->score,snap->alarmActive,snap->dataStale,
            snap->stateRoll[0],snap->stateRoll[1],snap->stateRoll[2],snap->stateRoll[3],snap->stateRoll[4],
            snap->alertCounts[1],snap->alertCounts[2]);
  if(link->haveState)
    fprintf(f,",\"sleeping\":%u,\"alarmAcked\":%u,\"brightnessPct\":%u,\"theme\":%u",
            link->lastState.sleeping,link->lastState.alarmAcked,
            link->lastState.brightnessPct,link->lastState.theme);
  fputs("}\n",f);
  fclose(f);
  (void)rename("/run/solari-panel/glance.json.tmp","/run/solari-panel/glance.json");
}

/* Purpose: run daemon's fail-soft polling and serial service loop. Input: config and password. Output: process status. */
static int runDaemon(const Config *config,const char *password){
  CURL *curl=NULL; PanelSnapshot latest; PanelParser parser; PanelLink link;
  int haveLatest=0,fd=-1;
  uint32_t now=nowMs(),nextPoll=now,nextFrame=now,nextSerial=now,authDelay=1u,transientDelay=1u,serialDelay=1u;
  int lastPollOk=0; uint32_t nextGlance=now;   /* at-a-glance matrix feed */
  memset(&latest,0,sizeof(latest)); memset(&link,0,sizeof(link)); link.config=config; panelParserInit(&parser);
  while(running){
    now=nowMs(); link.curl=curl;
    if(curl==NULL&&(int32_t)(now-nextPoll)>=0){curl=login(config,password);link.curl=curl;if(curl==NULL){nextPoll=now+authDelay*1000u;authDelay=authDelay<60u?authDelay*2u:60u;continue;}authDelay=1u;nextPoll=now;}
    if(curl!=NULL&&(int32_t)(now-nextPoll)>=0){
      HttpResponse response; char url[768]; CURLcode result;
      snprintf(url,sizeof(url),"%s/api/panel",config->apiBase); result=request(curl,url,NULL,&response);
      if(result==CURLE_OK&&response.status==401L){curl_easy_cleanup(curl);curl=login(config,password);link.curl=curl;if(curl!=NULL)result=request(curl,url,NULL,&response);}
      if(result==CURLE_OK&&response.status>=200L&&response.status<300L&&strncasecmp(trim(response.contentType),"application/json",16u)==0&&parseSnapshot(response.data,&latest)==0){
        haveLatest=1; lastPollOk=1; transientDelay=1u; nextPoll=now+config->pollSec*1000u;
        if(fd>=0){
          int64_t age=(int64_t)time(NULL)-(int64_t)latest.ts;
          latest.dataStale=(uint8_t)(latest.dataStale!=0u||age>35); latest.seq++;
          if(sendFrame(fd,&latest,1)!=0||forwardCommands(fd,response.data,&link)!=0){logMessage("serial write failure; reopening");close(fd);fd=-1;nextSerial=now+serialDelay*1000u;serialDelay=serialDelay<30u?serialDelay*2u:30u;}else nextFrame=now+config->snapshotSec*1000u;
        }
      }else if(response.status==401L){curl_easy_cleanup(curl);curl=NULL;link.curl=NULL;nextPoll=now+authDelay*1000u;authDelay=authDelay<60u?authDelay*2u:60u;}
      else{logMessage("panel fetch failed (curl=%d HTTP=%ld)",(int)result,response.status);lastPollOk=0;nextPoll=now+transientDelay*1000u;transientDelay=transientDelay<60u?transientDelay*2u:60u;}
    }
    if(fd<0&&(int32_t)(now-nextSerial)>=0){fd=openSerial(config->serialDev);if(fd<0){nextSerial=now+serialDelay*1000u;serialDelay=serialDelay<30u?serialDelay*2u:30u;}else{serialDelay=1u;nextSerial=now;panelParserInit(&parser);link.stateSeen=0;link.haveState=0;link.withholdingLogged=0;nextFrame=now;
      /* Ask the panel for HELLO + an immediate STATE (panelCtlForceReport):
       * without this, a daemon restart against an already-booted panel waits
       * out the 30 s heartbeat before the capability gate opens.           */
      {uint8_t frame[PANEL_HDR_SIZE+PANEL_CRC_SIZE];size_t n=panelEncodeFrame(PANEL_FT_HELLOREQ,NULL,0u,frame,sizeof(frame));if(n>0u)(void)writeAll(fd,frame,n);}}}
    if(fd>=0){
      if(readSerial(fd,&parser,&link)!=0){logMessage("serial read failure; reopening");close(fd);fd=-1;nextSerial=now+serialDelay*1000u;serialDelay=serialDelay<30u?serialDelay*2u:30u;continue;}
      if((int32_t)(now-nextFrame)>=0){if(haveLatest){int64_t age=(int64_t)time(NULL)-(int64_t)latest.ts;latest.dataStale=(uint8_t)(latest.dataStale!=0u||age>35);latest.seq++;}if(sendFrame(fd,&latest,haveLatest)!=0){logMessage("serial write failure; reopening");close(fd);fd=-1;nextSerial=now+serialDelay*1000u;serialDelay=serialDelay<30u?serialDelay*2u:30u;}nextFrame=now+config->snapshotSec*1000u;}
    }
    /* Feed the on-board glance matrix every 2 s (cosmetic, fail-silent). */
    if((int32_t)(now-nextGlance)>=0){glanceWrite(&latest,haveLatest,lastPollOk,&link,fd>=0);nextGlance=now+2000u;}
    sleepMs(20L);
  }
  if(fd>=0) close(fd);
  if(curl!=NULL) curl_easy_cleanup(curl);
  return 0;
}
/* Purpose: parse command-line configuration option and launch daemon. Input: argc/argv. Output: process exit code. */
#ifdef SOLARI_PANEL_TEST
int panelParseSnapshotForTest(const char *text,PanelSnapshot *snapshot){return parseSnapshot(text,snapshot);}
/* Test shim for forwardCommands: builds a PanelLink locally (the type is
 * private to this TU) so the test can exercise envelope navigation and the
 * stateSeen capability gate against a pipe fd. */
int panelForwardCommandsForTest(int fd,const char *text,int stateSeen){PanelLink link;memset(&link,0,sizeof(link));link.stateSeen=(uint8_t)stateSeen;return forwardCommands(fd,text,&link);}
#else
int main(int argc,char **argv){Config config;char password[512];const char *path=DEFAULT_CONFIG;if(argc==3&&strcmp(argv[1],"--config")==0)path=argv[2];else if(argc!=1){fprintf(stderr,"usage: %s [--config path]\n",argv[0]);return 2;}if(loadConfig(path,&config)!=0||readPassword(config.passFile,password,sizeof(password))!=0){logMessage("configuration or password file unavailable");return 1;}signal(SIGINT,stopSignal);signal(SIGTERM,stopSignal);signal(SIGPIPE,SIG_IGN);if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK)return 1;runDaemon(&config,password);curl_global_cleanup();memset(password,0,sizeof(password));return 0;}
#endif
