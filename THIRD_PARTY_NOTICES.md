# Third-party notices

wkMapZoom itself is MIT licensed ([LICENSE](LICENSE)). It contains or is derived from the following.

## wkVista

wkMapZoom's map drawing (`src/raster.c`: the drawing operations and what each does to a pixel), its true-colour output
(`src/present.c`), its full-colour maps (`src/colourmap.c`) and its Esc menu items (`src/escmenu.c`) are ported to C from
[wkVista](https://github.com/ropahektic/wkVista) (Rust) and changed: area averaging in RGB, the land's shrunk copies,
zooming out, a system-memory surface for the true-colour frame. The split of WA's drawing queue into screen and camera
entries is also wkVista's.

```
MIT License

Copyright (c) 2026 ropahektic

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## wkRGBA

Loading full-colour maps by handing WA a converted stand-in when it opens the file was first done by
[wkRGBA](https://github.com/rUNaW4y/wkRGBA) (rUNaW4y), as wkVista credits it. No wkRGBA code is used.

## MinHook

`third_party/minhook` is [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu, under its own BSD 2-clause
licence: [third_party/minhook/LICENSE.txt](third_party/minhook/LICENSE.txt).
