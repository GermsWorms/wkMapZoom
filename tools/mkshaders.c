/* mkshaders: compiles the pixel shaders of wkMapZoom's graphics-card drawing (src\\gpu.c) with d3dcompiler_47.dll at build time and writes src\shaders.h (the byte
 * code as arrays), so the module itself never loads a compiler inside WA.
 *   mkshaders <out.h> */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef struct { void **vt; } blob;
typedef HRESULT(WINAPI *compile_fn)(const void *, SIZE_T, const char *, const void *, void *, const char *, const char *, UINT, UINT, blob **, blob **);

/* s0: an 8-bit picture (palette indices, L8; or the index target). s1: WA's palette, 256 x 1.
 * A vertex's colour says what its rectangle is: red the see-through index / 255, alpha 1 when that index is see-through,
 * green a fill's index / 255, blue 1 for a fill (the picture is not used). Its second texture coordinate (zoomed frames)
 * is the part of the texture its picture lies in (samples are kept inside). c1.xy: what one screen pixel covers of the
 * texture, in texture coordinates (zoomed out).
 * The palette place is WA's own: index * 255/256 + 1/512 (its shader at 00648A18).
 *   idx_any: into the index target (the index in every channel), exact
 *   final:   the index target through the palette, onto the back buffer
 *   plain:   a true-colour picture as it is (the notice)
 *   colour:  the vertex's colour (other modules' triangles)
 *   rgb_any: straight through the palette (zoomed frames: what stays 1:1)
 *   avgN:    N x N samples through the palette, averaged, see-through as alpha (premultiplied) */
static const char *SRC =
    "sampler2D idx : register(s0); sampler2D pal : register(s1); float4 c1 : register(c1);\n"
    "float4 look(float i) { return tex2D(pal, float2(i * (255.0 / 256.0) + (1.0 / 512.0), 0.5)); }\n"
    "float pick(float2 uv, float4 col) { float i = tex2D(idx, uv).r; clip(abs(i - col.r) * 255.0 - 0.5 + (1.0 - col.a)); return lerp(i, col.g, col.b); }\n"
    "float4 idx_any(float2 uv : TEXCOORD0, float4 col : COLOR0) : COLOR { float i = pick(uv, col); return float4(i, i, i, 1); }\n"
    "float4 final(float2 uv : TEXCOORD0) : COLOR { return look(tex2D(idx, uv).r); }\n"
    "float4 plain(float2 uv : TEXCOORD0) : COLOR { return tex2D(idx, uv); }\n"
    "float4 colour(float4 col : COLOR0) : COLOR { return col; }\n"
    "float4 rgb_any(float2 uv : TEXCOORD0, float4 col : COLOR0) : COLOR { return float4(look(pick(uv, col)).rgb, 1); }\n"
    "#ifdef N\n"
    "float4 avg(float2 uv : TEXCOORD0, float4 lim : TEXCOORD1, float4 col : COLOR0) : COLOR {\n"
    "  float4 s = 0;\n"
    "  for (int j = 0; j < N; j++) for (int k = 0; k < N; k++) {\n"
    "    float2 p = clamp(uv + ((float2(k, j) + 0.5) / N - 0.5) * c1.xy, lim.xy, lim.zw);\n"
    "    float i = tex2Dlod(idx, float4(p, 0, 0)).r;\n"
    "    float4 c = tex2Dlod(pal, float4(i * (255.0 / 256.0) + (1.0 / 512.0), 0.5, 0, 0));\n"
    "    float a = (col.a > 0.5 && abs(i - col.r) * 255.0 < 0.5) ? 0.0 : 1.0;\n"
    "    s += float4(c.rgb * a, a); }\n"
    "  return s / (N * N); }\n"
    "#endif\n";

static int emit(FILE *out, compile_fn cf, const char *name, const char *entry, const char *profile, const char *n) {
    struct { const char *a, *b; } def[2] = {{n ? "N" : NULL, n}, {NULL, NULL}};
    blob *code = NULL, *err = NULL;
    const DWORD *p;
    SIZE_T size, i;
    HRESULT hr = cf(SRC, strlen(SRC), name, n ? (const void *)def : NULL, NULL, entry, profile, 0x1000 /* O3 */, 0, &code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "%s: %s\n", name, err ? (char *)((void *(WINAPI *)(void *))err->vt[3])(err) : "?");
        return 0;
    }
    p = (const DWORD *)((void *(WINAPI *)(void *))code->vt[3])(code);
    size = ((SIZE_T(WINAPI *)(void *))code->vt[4])(code);
    fprintf(out, "static const DWORD %s[] = {", name);
    for (i = 0; i < size / 4; i++) fprintf(out, "%s0x%08lx,", i % 8 ? " " : "\n    ", (unsigned long)p[i]);
    fprintf(out, "\n};\n");
    return 1;
}

int main(int argc, char **argv) {
    HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
    compile_fn cf = m ? (compile_fn)(void *)GetProcAddress(m, "D3DCompile") : NULL;
    FILE *out;
    int ok;
    if (argc < 2 || !cf) { fprintf(stderr, "usage: mkshaders out.h (needs d3dcompiler_47.dll)\n"); return 1; }
    out = fopen(argv[1], "w");
    if (!out) return 1;
    fprintf(out, "/* made by tools\\mkshaders.c (Direct3D 9 pixel shader byte code): do not edit */\n");
    ok = emit(out, cf, "ps_idx_any", "idx_any", "ps_2_0", NULL) && emit(out, cf, "ps_final", "final", "ps_2_0", NULL) &&
         emit(out, cf, "ps_plain", "plain", "ps_2_0", NULL) &&
         emit(out, cf, "ps_colour", "colour", "ps_2_0", NULL) &&
         emit(out, cf, "ps_rgb_any", "rgb_any", "ps_2_0", NULL) &&
         emit(out, cf, "ps_avg2", "avg", "ps_3_0", "2") && emit(out, cf, "ps_avg3", "avg", "ps_3_0", "3") &&
         emit(out, cf, "ps_avg4", "avg", "ps_3_0", "4") && emit(out, cf, "ps_avg6", "avg", "ps_3_0", "6") &&
         emit(out, cf, "ps_avg8", "avg", "ps_3_0", "8");
    fclose(out);
    return ok ? 0 : 2;
}
