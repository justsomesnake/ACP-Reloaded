ACP Reloaded 1.0.1-beta – Animus Control Panel for Assassin's Creed Origins (PC)
===============================================================================

Brings back the removed Animus Control Panel as an in-game overlay: live editing in a
"Hacked Animus" save, keyboard/mouse and full controller support.

INSTALL (Windows 10 / 11)
1. Copy dinput8.dll and ACPReloaded.dll into the game folder (next to ACOrigins.exe).
   Steam: ...\steamapps\common\Assassins Creed Origins\
2. Start the game, load a "Hacked Animus" save (press Q on a save in the load menu to hack it).
3. Open the panel: Shift+F2 (changeable in Settings) or hold View + Menu on the controller.

Linux / Proton / Steam Deck: same two files, plus this Steam launch option:
   WINEDLLOVERRIDES="dinput8=n,b" %command%

UNINSTALL
Delete dinput8.dll and ACPReloaded.dll from the game folder. Game files are never modified.
Settings, log, presets and save backups are in:
   Documents\Assassin's Creed Origins\ACP_Reloaded\

GOOD TO KNOW
- The panel only opens inside a running "Hacked Animus" save (can be changed in Settings).
- Game functions (time of day, XP/level/ability points) are experimental and must be enabled in
  Settings. Back up your saves first (button in the panel).
- Tested: Steam version on Windows 11. Ubisoft Connect version, Proton/Steam Deck and other
  overlays (ReShade, RTSS, Special K) are untested.
- Character choice (Hero General > Character & Companions) shows a portrait of each of the 21 ACP characters;
  "Show all characters" opens an overview to pick one.
- Expert mode lets you type values outside the ranges, but the game caps every ACP value at its own limits
  (e.g. movement speed 400 %). Extra speed (our own feature) goes up to 400 % in expert mode.
- Use only one frame rate limiter (ours, the game's, RTSS or the driver's).

USING OTHER MODS AT THE SAME TIME
- Ultimate ASI Loader users: do NOT copy our dinput8.dll. Rename ACPReloaded.dll to ACPReloaded.asi and put it
  where your ASI loader looks for plugins (game folder or "scripts" folder).
- Another mod that also needs a dinput8.dll: rename THAT mod's dinput8.dll to dinput8_chain.dll, then copy ours.
  Our loader starts the other mod first, then ACP Reloaded.
- Loading ACP Reloaded twice (both ways at once) is harmless: the second copy stays inactive.

KNOWN CONFLICTS / NOT TESTED
- Other frame rate limiters (RTSS, driver limit, Special K): use only one.
- ReShade, RTSS/Afterburner, Discord, NVIDIA/AMD overlays: should work side by side, not tested yet.
- Trainers or memory tools that change the same values (XP, time) may conflict; our game functions switch
  themselves off if the game code does not match the expected build.

KNOWN BEHAVIOUR
- Teleport (experimental): it moves you instantly, without the loading screen of fast travel. In rare cases
  people at the destination (vendors, blacksmith, ...) are not there yet - wait a moment or walk away and come back.
- Quick teleport on the map: with teleport switched on, set your marker on the world map and hold the right mouse
  button or X (controller) for about 1 s; the jump follows as soon as you close the map. Only counts while the map is
  open, so bow aiming and other hold actions in the world are not affected.
- Teleport into deep water puts you on the water surface. "Teleport to quest target" takes you to the nearest active
  objective of the tracked quest - also inside tombs or next to a moving person; skipping the way there can skip
  scripted events on that way.
- Extra speed (experimental, Hero General > Movement): plays only your character (surface swimming) or only your own
  horse/camel faster, so it looks fast-forwarded. Reset to normal when you stop swimming, dismount or switch it off.
- Teleport while on a boat: you are moved alone, the boat stays where it was (like getting off). Horses and
  camels come along when "Allow while mounted" is on.
- Screenshots: Settings > "Hide the ACP watermark" removes the faint ACP text the game shows in a "Hacked Animus"
  save (in play and in photo mode). Achievements, saves and values are not affected.
- The log (Documents\Assassin's Creed Origins\ACP_Reloaded\ACPReloaded.log) is capped at 1 MB and shows
  %USERPROFILE% instead of your user name.

ACP Reloaded is and stays free. If it helped you: https://ko-fi.com/mythenstudios - thank you!

License: GNU General Public License v3.0 (LICENSE). Source code: https://github.com/justsomesnake
Third-party components and licenses: see THIRD_PARTY.md and Cinzel_OFL.txt.

Unofficial fan-made mod. Not affiliated with or endorsed by Ubisoft. Assassin's Creed is a trademark of
Ubisoft Entertainment.
