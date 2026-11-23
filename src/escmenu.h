/* escmenu.h: a Resolution dropdown and a Fullscreen / Windowed switch in WA's Esc menu, applied live (wkMapZoom) */
#ifndef WKMAPZOOM_ESCMENU_H
#define WKMAPZOOM_ESCMENU_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

/* hooks WA 3.8.1's Esc menu; keep_setting 0: a resolution chosen is not written to WA's settings (test mode) */
int escmenu_install(uintptr_t base, void (*say)(const char *, ...), int keep_setting);
/* each frame: WA's runtime (the switches act on it) */
void escmenu_frame(void *runtime);
/* a message on WA's thread: the module's switch message runs the switch asked for (1: it was that message) */
int escmenu_message(const MSG *m);
/* for test mode: a resolution (w, h) or the display switch (w = 0) asked for, as a click in the menu would */
void escmenu_ask(int w, int h);
/* WA's Direct3D device is gone, or its Reset keeps failing (stuck) -- present.c's safety net, from inside WA's
 * show-frame: a new one made between frames; back: the device is there again (a new incident's tries) */
void escmenu_recover(int stuck);
void escmenu_device_back(void);
/* the game's end hooked and base/log set: needed by the recovery with or without the menu's items; 1: done */
int escmenu_base(uintptr_t base, void (*say)(const char *, ...));
/* for test mode: the device made anew as the recovery does, though it works */
void escmenu_test_recover(void);
/* for test mode: a resolution asked for even if it is not one of the monitor's modes (fullscreen: the rollback) */
void escmenu_test_any_size(int w, int h);
/* for test mode: a line in WA's chat (pin: the chat pinned open), the chat's sizes checked against the frame's, and the
 * chat's re-layout after a size change turned off (to see the old way) */
void escmenu_test_chat(const char *text, int pin);
void escmenu_test_chat_check(const char *when);
void escmenu_test_chatfix(int on);
/* for test mode: WA's Esc menu opened (OpenEscMenu), and the resolution list opened (as a click on its header) */
void escmenu_test_open(void);
void escmenu_test_list(void);

/* the module's own options, under a "Map zoom options" row of their own: each a row whose label shows its value and
 * whose click changes it (the menu is laid out again to show the change) */
typedef struct {
    int (*count)(void);
    void (*label)(int i, char *out, size_t n);
    void (*click)(int i);
} escmenu_options;
void escmenu_set_options(const escmenu_options *o);
/* for test mode: the options list opened (as a click on its row), and option i clicked */
void escmenu_test_options(void);
void escmenu_test_click(int i);

#endif
