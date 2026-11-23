/* present.h: true-colour output through WA's Direct3D 9 backends, and a safety net for their show-frame (wkMapZoom) */
#ifndef WKMAPZOOM_PRESENT_H
#define WKMAPZOOM_PRESENT_H
#include <stdint.h>

/* writes the frame to show, 0x00RRGGBB rows `pitch` bytes apart, w x h; 0 when it can't (WA shows its own) */
typedef int (*present_compose_fn)(uint8_t *bits, int pitch, int w, int h);

/* hooks WA 3.8.1's two Direct3D 9 "show frame" functions; 0 when their code is not as expected. compose NULL: the
 * safety net only (frames skipped while WA has no device), no true colour */
int present_install(uintptr_t base, present_compose_fn compose, void (*say)(const char *, ...));
/* the frame WA just drew has a true-colour version (shown by the next "show frame") */
void present_ready(void);
/* how many frames were shown in true colour (for the log) */
unsigned present_shown(void);
/* true colour is live: WA's Direct3D 9 show-frame ran in the last second and nothing stops the module's frame from
 * being shown (not failed, the back buffer's format usable, no frame of another size refused since one was shown) */
int present_live(void);
/* gone: called (from inside WA's show-frame, so it should only post a message) about once a second while WA has no
 * device (stuck 0), or once every 3 s while WA's Reset of its device keeps failing (stuck 1); back: the device is there
 * again after it was gone */
void present_on_gone(void (*gone)(int stuck), void (*back)(void));
/* the module's own system-memory surface released (before WA's display is set up again) */
void present_forget(void);
/* test mode: the next `frames` frames are shown as if WA had no device (skipped) */
void present_test_gone(int frames);

#endif
