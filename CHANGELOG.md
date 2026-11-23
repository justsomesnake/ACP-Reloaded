# Changelog

## 1.0.1-beta

**New**
- Hide the ACP watermark (Settings, with the game functions on): hides the faint ACP text the game shows in a
  "Hacked Animus" save, in play and in photo mode - for clean screenshots. Only changes how that text is drawn;
  achievements, saves and ACP values are not affected. Turning it off or unloading the overlay brings it back.

**Fixes**
- The log file no longer grows without limit: at 1 MB it is renamed to `ACPReloaded.old.log` (one old file kept).
- Paths in the log show `%USERPROFILE%` instead of your Windows user name, so a log can be shared safely.
- Fewer routine lines in the log (Uplay statistics every 10 minutes).

## 1.0.0-beta

First public release.

**Animus Control Panel**
- All 75+ ACP settings (every value of the game's ACP.ini), applied live in a "Hacked Animus" save through the game's own Uplay options event.
- Presets, search, "changed only" filter, undo (Ctrl+Z / Y), reset per value or page, "Apply now" for values that
  reload the world.
- Character choice with names and portraits for all 21 ACP characters, overview grid.
- Two themes: classic Uplay panel and an *Origins* theme modelled on the game's menus.
- Console-style controller navigation (D-pad/left stick, LB/RB pages, A/B/X/Y), keyboard and mouse.
- Configurable toggle key (default Shift+F2) and View + Menu on the controller; opens only in a running
  "Hacked Animus" save (optional).

**Quality of life**
- Frame rate limiter (steps or free value, background limit), FPS display and frame time graph.
- Contrast-adaptive sharpening and an optional custom colour filter.
- Save backup button, "create ACP.ini with original values".
- Loads via `dinput8.dll`, as an `.asi` plugin, or chained with another `dinput8.dll` mod.

**Experimental game functions** (opt-in, "Hacked Animus" save only, verified against game build 16965472)
- Exact time of day, hold time, own time-lapse clock while a quest locks the ACP day length.
- Grant XP, raise the level, grant ability points.
- Weather control (hold intensity, no sandstorms / sandstorms likely).
- Teleport to the map marker (also map icons, side quests, trials), to the tracked quest target, quick teleport from
  the world map (hold right mouse button or X), landing on the water surface, options for combat and mounts.
- Extra speed for surface swimming and your own horse/camel (up to 200 %, 400 % in expert mode).

**Known limits**
- Tested on the Steam version under Windows 11 only.
- The game caps every ACP value at its own limits (e.g. 400 % movement speed), also in expert mode.
- Teleporting on a boat moves you alone; rarely, people at the destination appear a moment late.
