# wkMapZoom

Zoom the map in and out in **Worms Armageddon 3.8.1**: hold **Ctrl** and turn the **mouse wheel**.
From **25%** (four times the map each way) to **800%**.

Only the map zooms. The team bars, turn timer, wind, messages, chat, Esc menu and weapon menu stay their normal
size, and a small number (for example `150%`) shows the zoom while you change it.

| 25% | 50% |
|---|---|
| ![25%](screenshots/zoom-025.png) | ![50%](screenshots/zoom-050.png) |
| **100%** | **200%** |
| ![100%](screenshots/zoom-100.png) | ![200%](screenshots/zoom-200.png) |
| **800%** | |
| ![800%](screenshots/zoom-800.png) | |

The same race map at each zoom: the HUD at the bottom (timer, team bar, wind) and the zoom number stay the same size,
while the map, the worm and its name zoom.

## Install

1. Download `wkMapZoom.dll` (and, if you want to change the settings, `wkMapZoom.ini`) from
   [`release/`](release/) and put them in your Worms Armageddon folder, next to `WA.exe`.
2. In WA: **Options > Advanced >** tick **Load WormKit modules**.
3. Start a game, hold Ctrl and scroll.

| | |
|---|---|
| **Ctrl + mouse wheel** | zoom in / out |
| **Ctrl + middle click** | back to 100% |

Uninstall: delete `wkMapZoom.dll`.

## Notes

- **WA 3.8.1 only.** The module checks `WA.exe` and does nothing on any other version.
- **Only your own screen changes.** No game state is touched, so online games and replays stay in sync, and other
  players don't need the module.
- **Zooming out shows you more of the map than other players see.** Say so before using it in league or competitive
  games, where it may count as an unfair advantage.
- Zoomed far out, WA has to draw a lot more of the map each frame (16x the area at 25%), so the frame rate can drop on
  big screens. `MaxOut=2` in `wkMapZoom.ini` stops at 50%.
- It doesn't touch WA's renderer, so it should work with any renderer, full screen or windowed, and through a
  resolution change in game. Tested so far: Direct3D 9, windowed.
- Don't run it together with another zoom module (wkVista, WormForge's zoom): they all take over WA's map drawing.
- Log: `%LOCALAPPDATA%\wkMapZoom\wkMapZoom.log`.

## Settings (`wkMapZoom.ini`, all optional)

| Key | Default | |
|---|---|---|
| `MaxOut` | `4` | how far out: 4 = 25% |
| `MaxIn` | `8` | how far in: 8 = 800% |
| `Step` | `1.15` | zoom per wheel notch |
| `Smooth` | `1` | glide to the new zoom (0 = jump) |
| `Filter` | `smooth` | `smooth`: each screen pixel is the average of the map pixels it covers, so thin walls stay visible zoomed out and pixels stay even zoomed in; `nearest`: plain pixel picking |
| `Indicator` | `zooming` | the zoom number: `zooming` (shown while zooming, then fades), `always` (stays while not at 100%), `off` |
| `IndicatorPlace` | `top-right` | or `top-left`, `top`, `bottom-right`, `bottom-left`, `bottom` |

## How it works

Each frame WA queues everything it draws and then draws the queue back to front. Queue entries of kinds 1, 4 and 7
are placed on the screen (the HUD); every other kind goes through the camera (the map: sky, land, water, worms,
weapons, names). While zoomed, wkMapZoom has WA draw the map entries, with WA's own code at WA's own scale, into a
bigger buffer that holds the whole area the zoomed view needs. It then scales that area onto WA's frame and lets WA
draw the HUD on top as usual. WA's camera clamp is given the size the zoomed view sees, so the camera reaches the
level's edges when zoomed in. The zoom number is drawn into WA's frame in WA's own palette.

Hooks (WA 3.8.1, checked by the exact SHA-256 of `WA.exe` and the original bytes at each hook point):
`00533DC0` GameRender, `00542350` RenderDrawingQueue, `00542F10` ClampCameraToBounds, `0056B810` the display's
rectangle fill (filled by worker threads while the map is drawn), plus a `WH_GETMESSAGE` hook on WA's thread for
Ctrl + wheel.

## Building

Needs a 32-bit MinGW gcc (for example `C:\MinGW\bin\gcc.exe`) and PowerShell:

```powershell
.\build.ps1                 # runs tests\*.c, then builds wkMapZoom.dll
.\install.ps1               # copies it into the Steam WA folder (-Game <folder> for another one)
.\install.ps1 -Remove
```

## Credits and licence

- The split of WA's drawing queue into screen entries (kinds 1, 4, 7) and camera entries was first done by
  [wkVista](https://github.com/ropahektic/wkVista) (MIT), which zooms in by redrawing the map itself.
- [MinHook](https://github.com/TsudaKageyu/minhook) (BSD 2-clause), in `third_party/minhook`.
- wkMapZoom is MIT licensed: see [LICENSE](LICENSE).
