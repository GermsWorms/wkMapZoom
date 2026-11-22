/* guard_test: the WA.exe check (image_guard.h) on Steam's WA.exe 3.8.1 (WA_EXE, else Steam's usual folder): the file
 * itself is Steam's (1); a copy with its "Steam" texts blanked, as GOG's edition is made, is the same game (2); a copy
 * with one byte of code changed, or with its sections moved, is not (0). Skipped where there is no such WA.exe. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/image_guard.h"

static unsigned char *load(const char *p, DWORD *n) {
    HANDLE f = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    unsigned char *b;
    LARGE_INTEGER s;
    if (f == INVALID_HANDLE_VALUE) return NULL;
    GetFileSizeEx(f, &s);
    b = (unsigned char *)malloc((size_t)s.QuadPart);
    if (!b || !ReadFile(f, b, (DWORD)s.QuadPart, n, NULL)) { free(b); b = NULL; }
    CloseHandle(f);
    return b;
}

static int check_copy(const unsigned char *b, DWORD n, const char *name, int want) {
    char p[MAX_PATH], why[256];
    FILE *f;
    int got;
    GetTempPathA(sizeof(p), p);
    strcat(p, name);
    f = fopen(p, "wb");
    if (!f) { printf("guard_test: can't write %s\n", p); return 0; }
    fwrite(b, 1, n, f);
    fclose(f);
    got = wa_supported_image(p, why, sizeof(why));
    DeleteFileA(p);
    printf("  %-28s -> %d (%s)\n", name, got, why);
    if (got != want) { printf("guard_test: %s gave %d, not %d\n", name, got, want); return 0; }
    return 1;
}

int main(void) {
    char path[MAX_PATH], why[256];
    unsigned char *b, *c;
    DWORD n = 0, i, blanked = 0;
    int ok = 1, kind;
    if (!GetEnvironmentVariableA("WA_EXE", path, sizeof(path)))
        strcpy(path, "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Worms Armageddon\\WA.exe");
    if (!(b = load(path, &n))) { printf("guard_test: skipped (no %s)\n", path); return 0; }
    kind = wa_supported_image(path, why, sizeof(why));
    printf("  %-28s -> %d (%s)\n", "WA.exe", kind, why);
    if (kind != 1) { printf("guard_test: skipped (%s is not Steam's WA.exe 3.8.1)\n", path); free(b); return 0; }
    c = (unsigned char *)malloc(n);
    memcpy(c, b, n);                        /* GOG's way: the "Steam" texts outside the code blanked */
    for (i = 0x21a000u; i + 5 <= n; i++)
        if (!memcmp(c + i, "Steam", 5)) { memset(c + i, 0, 5); blanked++; }
    printf("  (%lu \"Steam\" texts blanked)\n", (unsigned long)blanked);
    ok &= blanked > 0 && check_copy(c, n, "wa_guard_gog_like.exe", 2);
    memcpy(c, b, n);                        /* one byte of code changed */
    c[0x1000 + 0x1234] ^= 0x01;
    ok &= check_copy(c, n, "wa_guard_code_changed.exe", 0);
    memcpy(c, b, n);                        /* .data moved (its address in the section table) */
    {
        const DWORD pe = *(DWORD *)(c + 0x3c), opt = *(WORD *)(c + pe + 20);
        *(DWORD *)(c + pe + 24 + opt + 40 * 2 + 12) += 0x1000;
    }
    ok &= check_copy(c, n, "wa_guard_data_moved.exe", 0);
    ok &= check_copy(c, 300, "wa_guard_too_small.exe", 0);
    free(c);
    free(b);
    printf("guard_test: %s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
