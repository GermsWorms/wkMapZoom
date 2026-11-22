/* colourmap.h: full-colour maps (wkMapZoom) -- PNG / JPEG maps in User\SavedLevels shown in their own colours */
#ifndef WKMAPZOOM_COLOURMAP_H
#define WKMAPZOOM_COLOURMAP_H
#include <stdint.h>

/* WA's theme number by name ("Forest", "-Beach", ...), -1 for none */
int colourmap_theme(const char *name);
/* WA's CreateFileA / CreateFileW imports patched: a full-colour image under User\SavedLevels opens as a palette PNG WA
 * accepts (its "proxy"); converted maps get WA theme `theme` */
int colourmap_install(uintptr_t base, const char *wa_dir, int theme, void (*say)(const char *, ...));
/* the full-colour original of a game's land (w x h; snap: the land at the game's start, 0 = air), 0x01RRGGBB per land
 * pixel (0: none), or NULL when no full-colour map matches; free() it */
uint32_t *colourmap_match(int w, int h, const uint8_t *snap);

#endif
