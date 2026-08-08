/* panelHelpOverlay.h — render the current screen's two-line help overlay. */

#ifndef SOLARI_PANEL_HELP_OVERLAY_H
#define SOLARI_PANEL_HELP_OVERLAY_H

/* panelHelpOverlayDraw — draw help for flattened theme*3+screen index 0..11.
 * `t` is seconds since boot and drives any needed text scrolling. */
void panelHelpOverlayDraw(unsigned screenIndex, float t);

#endif /* SOLARI_PANEL_HELP_OVERLAY_H */
