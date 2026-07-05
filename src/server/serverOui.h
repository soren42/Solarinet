/* serverOui.h - curated, extensible OUI subset for discovery enrichment. */
#ifndef SOLARI_SERVER_OUI_H
#define SOLARI_SERVER_OUI_H

const char *lookupOui(const char *mac);
unsigned serverOuiSubsetSize(void);

#endif /* SOLARI_SERVER_OUI_H */
