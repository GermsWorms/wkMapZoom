/* present.h: true-colour output through WA's Direct3D 9 backends (wkMapZoom) */
#ifndef WKMAPZOOM_PRESENT_H
#define WKMAPZOOM_PRESENT_H
#include <stdint.h>

/* writes the frame to show, 0x00RRGGBB rows `pitch` bytes apart, w x h; 0 when it can't (WA shows its own) */
typedef int (*present_compose_fn)(uint8_t *bits, int pitch, int w, int h);

/* hooks WA 3.8.1's two Direct3D 9 "show frame" functions; 0 when their code is not as expected */
int present_install(uintptr_t base, present_compose_fn compose, void (*say)(const char *, ...));
/* the frame WA just drew has a true-colour version (shown by the next "show frame") */
void present_ready(void);
/* how many frames were shown in true colour (for the log) */
unsigned present_shown(void);

#endif
