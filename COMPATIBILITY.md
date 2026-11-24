# wkMapZoom with other WormKit modules

What was tested, with what result, failures and gaps included. Worms Armageddon 3.8.1 (Steam), Graphics API Direct3D 9
(shader), in a window, vsync off, full detail, in an isolated copy of the game playing a replay.

"Check" is wkMapZoom's own picture check: every drawing call is made a second time by WA alone into a second frame,
and the graphics card's picture is compared with that frame pixel by pixel.

These runs show that the modules load and run together, that the picture stays WA's, and what the frame rate does.
They do **not** exercise most of the other modules' own features (see "Not tested yet").

## Each module with wkMapZoom (1280x720, 100% zoom, 20 seconds each)

| Module | fps without wkMapZoom | fps with wkMapZoom | The graphics card drew | Check | Notes |
|---|---|---|---|---|---|
| (wkMapZoom alone) | 994 | 2226 | every frame | 1200 frames, 0 differ |  |
| wkRemapKeys | 914 | 2244 | every frame | 1200 frames, 0 differ |  |
| wkRealTime 0.0.9.2 (wkZRealTime) | 921 | 2268 | every frame | 1200 frames, 0 differ |  |
| WormForge 0.9.9 (wkLua) | 879 | 2043 | every frame | 1200 frames, 0 differ | no mods enabled; the notice about WormForge is shown at the start |
| wkVista 0.1.4 | 291 | 2252 | every frame | 1200 frames, 0 differ | at 1x, an ordinary map; wkVista's own true-colour step is not run in the frames the graphics card draws (same picture) |
| wkZoom 0.2.0 | 848 | 2186 | every frame | 1200 frames, 0 differ | default scale |
| wkZoom 0.2.0 with `[Camera] Scale=2` | 1361 to 1516 | 1546 | none (it stood aside) |  | wkZoom draws the zoomed picture itself: wkMapZoom's drawing stands aside after 8 frames, and the picture is wkZoom's |
| wkRGBA (repo build) | 896 | 2176 | every frame | 1200 frames, 0 differ | no full-colour map in this run |
| wkJellyWorm 3.8.1 v1 | 934 | 2528 | every frame | 800 frames, 0 differ | its own renderer is off by default; it hooks WA's drawing queue, so wkMapZoom's zoom is not put in beside it (see below) |
| wkTerrainSync 1.2.6.1 | 1014 | 2264 | every frame | 1200 frames, 0 differ |  |
| wkTrackMeBetter 3.8.1 | 916 | 2227 | every frame | 1200 frames, 0 differ |  |
| wkPin 1.0.0.8 | 948 | 2198 | every frame | 1200 frames, 0 differ |  |
| wkVolumeControl | 975 | 2311 | every frame | 1200 frames, 0 differ |  |
| wkWall2Wall | 874 | 2245 | every frame | 1200 frames, 0 differ |  |
| wkSuperFrontend | 915 | 2208 | every frame | 1200 frames, 0 differ |  |
| wkHelp | 840 | 2209 | every frame | 1200 frames, 0 differ |  |
| wkTrackTime | 898 | 2211 | every frame | 1200 frames, 0 differ |  |
| wkPlaceWorms | 951 | 2285 | every frame | 1200 frames, 0 differ |  |
| wkRopeRaceDirection | 735 | 2192 | every frame | 1200 frames, 0 differ |  |
| wkSetRes (wkSetResCustom) | 991 | 2217 | every frame | 1000 frames, 0 differ |  |
| wkGolf | 793 | 1461 | every frame | 1000 frames, 0 differ |  |
| wkCrateEditor 1.0 | 937 | 2282 | every frame | 1200 frames, 0 differ |  |
| wkD3D9Wnd 0.7.1.1 | 929 | 2207 | every frame | 1200 frames, 0 differ | made for WA 3.7; nothing seen of it on 3.8.1 |

No crash and no ERRORLOG.TXT in any of these runs.

## Several at once

| Loaded together | fps | |
|---|---|---|
| wkMapZoom + wkFastDraw | 2283 | the graphics card drew every frame |
| the same + WormForge 0.9.9 | 2090 | the graphics card drew every frame; the notice about WormForge is shown |
| the same + wkVista 0.1.4 | 2218 | the graphics card drew every frame |
| wkMapZoom, wkFastDraw, wkSpeedometer, wkModMenu, wkRealTime, wkTerrainSync, wkTrackMeBetter, wkPin, wkVolumeControl, wkWall2Wall, wkSuperFrontend, wkRemapKeys | 1297 | the graphics card drew every frame |

Zoomed, with the check on: wkMapZoom at 35% with wkSpeedometer, wkModMenu and wkFastDraw (1920x1080): every frame
zoomed, 0 pixels differ in the 200 frames compared. A game played to its end at 50% with those three, the Mods panel
used on the way (the drawing switched off and on in it): WA closed by itself, 0 pixels differ in 800 zoomed frames.

## Other modules that hook the same functions

- **wkJellyWorm** hooks WA's drawing queue, which wkMapZoom's zoom needs to itself. wkJellyWorm loads first, so beside
  it wkMapZoom does not put its zoom in (the log says so): the map stays at 100%, and the graphics card still draws
  the picture.
- **wkVista, wkZoom, WormForge's zoom** are zoom modules too. They ran beside wkMapZoom at 1:1 without a crash, but two
  modules cannot both zoom the map: keep one of them in the WA folder. When wkZoom zooms the map itself, the picture
  arrives almost whole from it and wkMapZoom's drawing stands aside.
- **wkSpeedometer, wkModMenu** look for wkMapZoom and draw through it (the dial, the line, the Mods panel).
- **wkFastDraw** speeds up WA's own drawing on the CPU. With wkMapZoom the graphics card draws those frames instead;
  wkFastDraw still helps in the frames WA draws itself.

## How the drawing keeps out of the way

- It changes nothing when WA loads the module; it goes in when the first game is on the screen, so modules that check
  WA's code or function tables at load see WA's own.
- When frames arrive almost whole from another module, with only the fixed parts drawn by WA (wkZoom zoomed in), the
  drawing stands aside after 8 such frames and tries again now and then.
- A module that works on WA's finished picture in WA's show-frame step is overridden in the frames the graphics card
  draws (its step is not run there); a notice on the screen says so for 15 seconds, and the log. Seen with WormForge
  0.9.9 (`wkLua.dll`); with no mods enabled nothing of it was missing from the picture.
- Frames that WA's Export Video reads are drawn by WA itself. Screenshots get the graphics card's picture read back.
- What another module draws straight into WA's frame is laid over the graphics card's picture.

## Not tested yet

- **The other drawing modules' own features with wkMapZoom loaded:** wkVista's zoom and full-colour maps, wkZoom zoomed
  out and its HUD scale, WormForge with mods enabled (its sprites, fog, zoom), wkRGBA with a full-colour map,
  wkJellyWorm's renderer.
- The replays used hardly touch what many of these modules are for (placing worms, golf, crates, help screens): the
  runs show that they load and run beside wkMapZoom, not that every feature of theirs was drawn.
- Fullscreen, real games (replays only so far), online games, two games in a row without closing WA, weak or built-in
  graphics, GOG's WA, Wine.
- Could not be had from an author's own GitHub, so not tried: RubberWorm, wkKick38, wkMapGEN, wkFOW, wkFileOverride,
  wkPackets, wkWormOrder, wkBigLobby. wkBindKeys and wkWormNAT2 need another DLL beside them and do not start in the
  test copy.
