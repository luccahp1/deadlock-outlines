# Handoff: deadlock-outlines

## Current state
- Working Win32 app in `src/main.cpp`, built with `build.cmd` (MSVC, static CRT, one exe of about 240 KB).
- Tested against copies of real config files using `--root`. Toggles, sliders, reset, color and repeated saves produce correct edits, and the autoexec block does not duplicate.
- CI builds on every push. Pushing a `v*` tag attaches the exe to a GitHub release.

## Next steps
- Make the UI say which settings only apply offline (hideout, bot matches), so nobody expects the glow online.

## Findings from in-game testing (2026-11-13)
- `cl_glow_brightness` visibly works, but only on the local hideout and bot matches. Joining a Valve server (friend's lobby, matchmaking) puts it back to default. No console error is logged.
- Reason: most of the outline and glow convars are registered with FCVAR_CHEAT (0x4000), several also with FCVAR_DEVELOPMENTONLY (0x2). Flags read from the ConVar registration calls in `client.dll`:
  - `cl_glow_brightness` 0x4000
  - `r_citadel_npr_outlines` 0x4002
  - `citadel_player_glow_disabled` 0x84000
  - `citadel_player_outline_enemies` 0x2
- With `sv_cheats 0` on official servers the client resets cheat convars, so there is no config-file fix. Getting around it would mean bypassing cheat protection, which is out of scope and a ban risk.
- `r_citadel_selection_outline2_width` changed nothing visible, even offline.
- The settings that do stick online are archived user settings (0x8080): `citadel_enemy_ui_color_*`, `citadel_enemy_damage_delta_color_*`, `citadel_enemy_zipline_color_*`.

## Notes / gotchas
- Saved user values in `user_convars*.vcfg` override the `gameinfo.gi` ConVars block. That is why the color goes in the vcfg files and in autoexec.
- `autoexec.cfg` is run on every launch (the console log shows `execing autoexec.cfg`).
- Deadlock Mod Manager manages the `SearchPaths` block in `gameinfo.gi`. With `autoResetGameinfoOnExit` off, it leaves the ConVars block alone.
- Running process names are `project8.exe` and `deadlock.exe`.
- `.bak` files are made once and never overwritten. They hold the originals for "Restore originals".
