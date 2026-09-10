#define _POSIX_C_SOURCE 200809L
#include "listener.h"
#include "../protocol.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define HANDSHAKE_MS 10000u
#define READ_IDLE_MS 90000u
#ifndef WRITE_MS
#define WRITE_MS 5000u /* test builds override (-DWRITE_MS=…) to exercise the
                        * stalled-peer deadline without a 5 s wall-clock wait */
#endif
#define DENY_MAX 256u
#define SERIAL_HEX_MAX 256u
#ifndef NI_MAXHOST
#define NI_MAXHOST 1025
#endif

typedef struct {
  int fd;
  SSL *ssl;
  uint32_t started;
  uint32_t lastRead;
  char peer[NI_MAXHOST];
  PanelParser parser;
} TlsPeer;

struct PanelListener {
  int fd;
  SSL_CTX *ctx;
  TlsPeer *pending;
  TlsPeer *active;
  char allowedSan[256];
  char *denied[DENY_MAX];
  size_t deniedCount;
  uint64_t generation;
  unsigned int port;
  PanelListenerFrameFn frameFn;
  void *frameUser;
  PanelListenerLogFn logFn;
  void *logUser;
};

static int expired(uint32_t now,uint32_t deadline){return (int32_t)(now-deadline)>=0;}
static void emit(PanelListener *l,const char *message){if(l->logFn!=NULL)l->logFn(message,l->logUser);}
static void peerFree(TlsPeer *peer){if(peer==NULL)return;if(peer->ssl!=NULL){SSL_set_quiet_shutdown(peer->ssl,1);SSL_free(peer->ssl);}if(peer->fd>=0)close(peer->fd);memset(&peer->parser,0,sizeof(peer->parser));free(peer);}
static int setNonblock(int fd){int flags=fcntl(fd,F_GETFL,0);return flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0?-1:0;}
/* safeFile — "or stricter" is a forbidden-bit mask, not an exact mode: the
 * private key may carry NO group/other bits and no execute bit (0600, 0400);
 * public certificates/CA may be group/other-READABLE but never writable or
 * executable by anyone (0644, 0640, 0444, ...). A 0644 key is a leak and a
 * 0400 key is fine — the old exact-mode check had both backwards. */
static int safeFile(const char *path,mode_t forbidden){struct stat st;if(path==NULL||path[0]=='\0'||stat(path,&st)!=0||!S_ISREG(st.st_mode))return -1;if((st.st_mode&0777&forbidden)!=0)return -1;if(st.st_uid!=0&&st.st_uid!=geteuid())return -1;return 0;}
static char *normalizedHex(const char *source){const char *p=source;char *out;size_t n=0u,i;while(*p=='0')++p;for(i=0u;p[i]!='\0';++i){if(!isxdigit((unsigned char)p[i]))return NULL;++n;}if(n==0u){out=malloc(2u);if(out!=NULL)strcpy(out,"0");return out;}out=malloc(n+1u);if(out==NULL)return NULL;for(i=0u;i<n;++i)out[i]=(char)toupper((unsigned char)p[i]);out[n]='\0';return out;}
static int loadDenylist(PanelListener *l,const char *path){FILE *f;char line[512];if(path==NULL||path[0]=='\0'){emit(l,"denylist not configured; using empty denylist");return 0;}f=fopen(path,"r");if(f==NULL){char message[768];if(errno==ENOENT){snprintf(message,sizeof(message),"denylist %s missing; using empty denylist",path);emit(l,message);return 0;}snprintf(message,sizeof(message),"denylist %s unreadable: %s",path,strerror(errno));emit(l,message);return -1;}while(fgets(line,sizeof(line),f)!=NULL){char *start=line,*end,*hex;while(isspace((unsigned char)*start))++start;end=start+strlen(start);while(end>start&&isspace((unsigned char)end[-1]))--end;*end='\0';if(*start=='\0'||*start=='#')continue;if(l->deniedCount==DENY_MAX||(hex=normalizedHex(start))==NULL){emit(l,"denylist contains an invalid or excess serial");fclose(f);return -1;}l->denied[l->deniedCount++]=hex;}if(ferror(f)){emit(l,"denylist read failed");fclose(f);return -1;}fclose(f);return 0;}
static void sanitize(const char *source,char *out,size_t cap){size_t i=0u;if(cap==0u)return;while(*source!='\0'&&i+1u<cap){unsigned char c=(unsigned char)*source++;out[i++]=(char)(isalnum(c)||strchr(" .:=/-",c)!=NULL?c:'_');}out[i]='\0';}
static void identity(X509 *cert,char *out,size_t cap){char cn[256]="not-present",san[256]="not-present",raw[600];X509_NAME *name;GENERAL_NAMES *names;int i;if(cert==NULL){snprintf(out,cap,"certificate-not-present");return;}name=X509_get_subject_name(cert);if(name!=NULL&&X509_NAME_get_text_by_NID(name,NID_commonName,cn,(int)sizeof(cn))<0)strcpy(cn,"not-present");names=X509_get_ext_d2i(cert,NID_subject_alt_name,NULL,NULL);if(names!=NULL){for(i=0;i<sk_GENERAL_NAME_num(names);++i){GENERAL_NAME *entry=sk_GENERAL_NAME_value(names,i);ASN1_STRING *value=NULL;if(entry->type==GEN_DNS)value=entry->d.dNSName;else if(entry->type==GEN_URI)value=entry->d.uniformResourceIdentifier;if(value!=NULL){int n=ASN1_STRING_length(value);if(n>(int)sizeof(san)-1)n=(int)sizeof(san)-1;memcpy(san,ASN1_STRING_get0_data(value),(size_t)n);san[n]='\0';break;}}GENERAL_NAMES_free(names);}snprintf(raw,sizeof(raw),"SAN=%.255s CN=%.255s",san,cn);sanitize(raw,out,cap);}
static int hasClientEku(X509 *cert){EXTENDED_KEY_USAGE *eku=X509_get_ext_d2i(cert,NID_ext_key_usage,NULL,NULL);int i,found=0;if(eku==NULL)return 0;for(i=0;i<sk_ASN1_OBJECT_num(eku);++i)if(OBJ_obj2nid(sk_ASN1_OBJECT_value(eku,i))==NID_client_auth){found=1;break;}EXTENDED_KEY_USAGE_free(eku);return found;}
static int sanMatches(X509 *cert,const char *allowed){GENERAL_NAMES *names=X509_get_ext_d2i(cert,NID_subject_alt_name,NULL,NULL);int i,matched=0;if(names==NULL)return 0;for(i=0;i<sk_GENERAL_NAME_num(names);++i){GENERAL_NAME *name=sk_GENERAL_NAME_value(names,i);ASN1_STRING *s=NULL;if(name->type==GEN_DNS)s=name->d.dNSName;else if(name->type==GEN_URI)s=name->d.uniformResourceIdentifier;if(s!=NULL&&(size_t)ASN1_STRING_length(s)==strlen(allowed)&&memcmp(ASN1_STRING_get0_data(s),allowed,strlen(allowed))==0){matched=1;break;}}GENERAL_NAMES_free(names);return matched;}
static int serialDenied(PanelListener *l,X509 *cert){ASN1_INTEGER *serial=X509_get_serialNumber(cert);BIGNUM *bn=ASN1_INTEGER_to_BN(serial,NULL);char *raw=NULL,*hex=NULL;size_t i;int found=0;if(bn==NULL)return 1;raw=BN_bn2hex(bn);BN_free(bn);if(raw!=NULL)hex=normalizedHex(raw);OPENSSL_free(raw);if(hex==NULL)return 1;for(i=0u;i<l->deniedCount;++i)if(strcmp(hex,l->denied[i])==0){found=1;break;}free(hex);return found;}
static int authorized(PanelListener *l,TlsPeer *peer){X509 *cert=SSL_get1_peer_certificate(peer->ssl);int ok=cert!=NULL&&SSL_get_verify_result(peer->ssl)==X509_V_OK&&hasClientEku(cert)&&sanMatches(cert,l->allowedSan)&&!serialDenied(l,cert);X509_free(cert);return ok;}
/* authReason — the specific check authorized() failed, re-run in order. Only
 * meaningful after a COMPLETED handshake; handshake-stage failures pass their
 * own reason to refuse() instead (the OpenSSL verify-result string when the
 * chain was the problem, a plain handshake reason otherwise). */
static const char *authReason(PanelListener *l,TlsPeer *peer){X509 *cert=SSL_get1_peer_certificate(peer->ssl);long verdict=SSL_get_verify_result(peer->ssl);const char *reason;if(cert==NULL)reason="no client certificate";else if(verdict!=X509_V_OK)reason=X509_verify_cert_error_string(verdict);else if(!hasClientEku(cert))reason="EKU lacks clientAuth";else if(!sanMatches(cert,l->allowedSan))reason="SAN not authorized";else if(serialDenied(l,cert))reason="serial denylisted";else reason="not authorized";X509_free(cert);return reason;}
static void refuse(PanelListener *l,TlsPeer *peer,const char *reason){char who[512],why[128],message[896];X509 *cert=peer->ssl!=NULL?SSL_get1_peer_certificate(peer->ssl):NULL;identity(cert,who,sizeof(who));X509_free(cert);sanitize(reason!=NULL?reason:"unknown",why,sizeof(why));snprintf(message,sizeof(message),"TLS client refused peer=%.127s identity=%.500s reason=%.127s",peer->peer,who,why);emit(l,message);}
static TlsPeer *acceptPeer(PanelListener *l,uint32_t now){struct sockaddr_storage address;socklen_t length=sizeof(address);TlsPeer *peer;int fd=accept(l->fd,(struct sockaddr *)&address,&length);if(fd<0)return NULL;peer=calloc(1u,sizeof(*peer));if(peer==NULL||setNonblock(fd)!=0){close(fd);free(peer);return NULL;}peer->fd=fd;peer->started=now;peer->lastRead=now;panelParserInit(&peer->parser);if(getnameinfo((struct sockaddr *)&address,length,peer->peer,sizeof(peer->peer),NULL,0,NI_NUMERICHOST)!=0)strcpy(peer->peer,"unknown");peer->ssl=SSL_new(l->ctx);if(peer->ssl==NULL){peerFree(peer);return NULL;}SSL_set_fd(peer->ssl,peer->fd);SSL_set_accept_state(peer->ssl);return peer;}
static void dispatch(uint8_t type,const uint8_t *payload,size_t length,void *user){PanelListener *l=user;if(l->active!=NULL&&l->frameFn!=NULL)l->frameFn(type,payload,length,l->generation,l->frameUser);}
static int sslWrite(TlsPeer *peer,const uint8_t *bytes,size_t length){size_t sent=0u;uint32_t deadline;struct timespec ts;if(clock_gettime(CLOCK_MONOTONIC,&ts)!=0)return -1;deadline=(uint32_t)((uint64_t)ts.tv_sec*1000u+(uint64_t)ts.tv_nsec/1000000u)+WRITE_MS;while(sent<length){int n;ERR_clear_error();n=SSL_write(peer->ssl,bytes+sent,(int)(length-sent));if(n>0){sent+=(size_t)n;continue;}{int e=SSL_get_error(peer->ssl,n);struct pollfd pfd;uint32_t now;if(e!=SSL_ERROR_WANT_READ&&e!=SSL_ERROR_WANT_WRITE)return -1;if(clock_gettime(CLOCK_MONOTONIC,&ts)!=0)return -1;now=(uint32_t)((uint64_t)ts.tv_sec*1000u+(uint64_t)ts.tv_nsec/1000000u);if(expired(now,deadline))return -1;pfd.fd=peer->fd;pfd.events=(short)(e==SSL_ERROR_WANT_READ?POLLIN:POLLOUT);pfd.revents=0;if(poll(&pfd,1,50)<0&&errno!=EINTR)return -1;}}return 0;}
static int sendHello(PanelListener *l){uint8_t frame[PANEL_HDR_SIZE+PANEL_CRC_SIZE];size_t n=panelEncodeFrame(PANEL_FT_HELLOREQ,NULL,0u,frame,sizeof(frame));return n==0u||sslWrite(l->active,frame,n)!=0?-1:0;}

PanelListener *panelListenerCreate(const PanelListenerConfig *config,PanelListenerFrameFn frameFn,void *frameUser,PanelListenerLogFn logFn,void *logUser){PanelListener *l=calloc(1u,sizeof(*l));struct addrinfo hints,*addresses=NULL,*it;char port[16];int yes=1,lastError=0;if(l==NULL)return NULL;l->fd=-1;l->frameFn=frameFn;l->frameUser=frameUser;l->logFn=logFn;l->logUser=logUser;if(config==NULL||config->address==NULL||config->address[0]=='\0'||config->certificate==NULL||config->privateKey==NULL||config->clientCa==NULL||config->allowedSan==NULL||config->allowedSan[0]=='\0'){emit(l,"listener configuration incomplete");panelListenerDestroy(l);return NULL;}if(strcmp(config->address,"0.0.0.0")==0||strcmp(config->address,"::")==0){emit(l,"refusing wildcard listenAddr");panelListenerDestroy(l);return NULL;}if(safeFile(config->certificate,(mode_t)0133)!=0||safeFile(config->privateKey,(mode_t)0177)!=0||safeFile(config->clientCa,(mode_t)0133)!=0){emit(l,"TLS files must be regular and root-or-daemon-owned; key mode 0600 or stricter, certificate/CA 0644 or stricter");panelListenerDestroy(l);return NULL;}snprintf(l->allowedSan,sizeof(l->allowedSan),"%s",config->allowedSan);if(strlen(config->allowedSan)>=sizeof(l->allowedSan)||loadDenylist(l,config->denylist)!=0){panelListenerDestroy(l);return NULL;}l->ctx=SSL_CTX_new(TLS_server_method());if(l->ctx==NULL||SSL_CTX_set_min_proto_version(l->ctx,TLS1_2_VERSION)!=1||SSL_CTX_set_cipher_list(l->ctx,"ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256")!=1||SSL_CTX_set_ciphersuites(l->ctx,"TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256")!=1||SSL_CTX_use_certificate_chain_file(l->ctx,config->certificate)!=1||SSL_CTX_use_PrivateKey_file(l->ctx,config->privateKey,SSL_FILETYPE_PEM)!=1||SSL_CTX_check_private_key(l->ctx)!=1||SSL_CTX_load_verify_locations(l->ctx,config->clientCa,NULL)!=1){emit(l,"TLS context/certificate setup failed");panelListenerDestroy(l);return NULL;}SSL_CTX_set_verify(l->ctx,SSL_VERIFY_PEER|SSL_VERIFY_FAIL_IF_NO_PEER_CERT,NULL);SSL_CTX_set_verify_depth(l->ctx,8);memset(&hints,0,sizeof(hints));hints.ai_family=AF_UNSPEC;hints.ai_socktype=SOCK_STREAM;hints.ai_flags=AI_NUMERICSERV;snprintf(port,sizeof(port),"%u",config->port);if(getaddrinfo(config->address,port,&hints,&addresses)!=0){emit(l,"listenAddr resolution failed");panelListenerDestroy(l);return NULL;}for(it=addresses;it!=NULL;it=it->ai_next){l->fd=socket(it->ai_family,it->ai_socktype,it->ai_protocol);if(l->fd<0){lastError=errno;continue;}(void)setsockopt(l->fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));if(bind(l->fd,it->ai_addr,it->ai_addrlen)==0&&listen(l->fd,4)==0&&setNonblock(l->fd)==0)break;lastError=errno;close(l->fd);l->fd=-1;}freeaddrinfo(addresses);if(l->fd<0){char message[256];snprintf(message,sizeof(message),"listener bind failed: %s",strerror(lastError));emit(l,message);panelListenerDestroy(l);return NULL;}{struct sockaddr_storage sa;socklen_t n=sizeof(sa);l->port=config->port;if(getsockname(l->fd,(struct sockaddr *)&sa,&n)==0)l->port=sa.ss_family==AF_INET?(unsigned int)ntohs(((struct sockaddr_in *)&sa)->sin_port):(unsigned int)ntohs(((struct sockaddr_in6 *)&sa)->sin6_port);}{char message[320];/* The runbook quotes this line; it reports the RESOLVED port so a
   port-0 (test) bind journals the real one. */snprintf(message,sizeof(message),"TLS listener ready on %s:%u",config->address,l->port);emit(l,message);}return l;}

void panelListenerService(PanelListener *l,uint32_t now){if(l==NULL)return;if(l->pending==NULL)l->pending=acceptPeer(l,now);if(l->pending!=NULL){int result;ERR_clear_error();result=SSL_accept(l->pending->ssl);if(result==1){if(!authorized(l,l->pending)){refuse(l,l->pending,authReason(l,l->pending));peerFree(l->pending);l->pending=NULL;}else{char who[512],message[768];X509 *cert=SSL_get1_peer_certificate(l->pending->ssl);identity(cert,who,sizeof(who));X509_free(cert);if(l->active!=NULL)peerFree(l->active);l->active=l->pending;l->pending=NULL;++l->generation;panelParserInit(&l->active->parser);snprintf(message,sizeof(message),"TLS panel connected peer=%.127s identity=%.500s generation=%llu",l->active->peer,who,(unsigned long long)l->generation);emit(l,message);if(sendHello(l)!=0){peerFree(l->active);l->active=NULL;}}}else{int error=SSL_get_error(l->pending->ssl,result);if(error!=SSL_ERROR_WANT_READ&&error!=SSL_ERROR_WANT_WRITE){long verdict=SSL_get_verify_result(l->pending->ssl);refuse(l,l->pending,verdict!=X509_V_OK?X509_verify_cert_error_string(verdict):"handshake failed");peerFree(l->pending);l->pending=NULL;}else if(expired(now,l->pending->started+HANDSHAKE_MS)){refuse(l,l->pending,"handshake deadline exceeded");peerFree(l->pending);l->pending=NULL;}}}if(l->active!=NULL){uint8_t bytes[512];int n;/* A refused handshake above leaves entries in the thread error queue, and
   * SSL_get_error consults the queue BEFORE rwstate — without this clear a
   * healthy active peer's WANT_READ is misread as SSL_ERROR_SSL and evicted. */
  ERR_clear_error();n=SSL_read(l->active->ssl,bytes,sizeof(bytes));if(n>0){l->active->lastRead=now;panelParserFeed(&l->active->parser,bytes,(size_t)n,now,dispatch,l);}else{int error=SSL_get_error(l->active->ssl,n);if((error!=SSL_ERROR_WANT_READ&&error!=SSL_ERROR_WANT_WRITE)||expired(now,l->active->lastRead+READ_IDLE_MS)){peerFree(l->active);l->active=NULL;}}}}
int panelListenerWrite(PanelListener *l,const uint8_t *bytes,size_t length){if(l==NULL||l->active==NULL)return -1;if(sslWrite(l->active,bytes,length)!=0){peerFree(l->active);l->active=NULL;return -1;}return 0;}
int panelListenerConnected(const PanelListener *l){return l!=NULL&&l->active!=NULL;}
uint64_t panelListenerGeneration(const PanelListener *l){return l!=NULL?l->generation:0u;}
unsigned int panelListenerPort(const PanelListener *l){return l!=NULL?l->port:0u;}
void panelListenerDestroy(PanelListener *l){size_t i;if(l==NULL)return;peerFree(l->pending);peerFree(l->active);if(l->fd>=0)close(l->fd);SSL_CTX_free(l->ctx);for(i=0u;i<l->deniedCount;++i)free(l->denied[i]);free(l);}
