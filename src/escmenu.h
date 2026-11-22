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
