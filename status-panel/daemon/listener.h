#ifndef SOLARI_PANEL_LISTENER_H
#define SOLARI_PANEL_LISTENER_H

#include <stddef.h>
#include <stdint.h>

typedef struct PanelListener PanelListener;
typedef void (*PanelListenerFrameFn)(uint8_t type, const uint8_t *payload,
                                    size_t length, uint64_t generation,
                                    void *user);
typedef void (*PanelListenerLogFn)(const char *message, void *user);

typedef struct {
  const char *address;
  unsigned int port;
  const char *certificate;
  const char *privateKey;
  const char *clientCa;
  const char *allowedSan;
  const char *denylist;
} PanelListenerConfig;

PanelListener *panelListenerCreate(const PanelListenerConfig *config,
                                   PanelListenerFrameFn frameFn,
                                   void *frameUser,
                                   PanelListenerLogFn logFn,
                                   void *logUser);
void panelListenerService(PanelListener *listener, uint32_t now);
int panelListenerWrite(PanelListener *listener, const uint8_t *bytes,
                       size_t length);
int panelListenerConnected(const PanelListener *listener);
uint64_t panelListenerGeneration(const PanelListener *listener);
unsigned int panelListenerPort(const PanelListener *listener);
void panelListenerDestroy(PanelListener *listener);

#endif
