# Handoff: deadlock-outlines

## Current state
- Working Win32 app in `src/main.cpp`, built with `build.cmd` (MSVC, static CRT, one exe of about 240 KB).
- Tested against copies of real config files using `--root`. Toggles, sliders, reset, color and repeated saves produce correct edits, and the autoexec block does not duplicate.
- CI builds on every push. Pushing a `v*` tag attaches the exe to a GitHub release.

## Next steps
- Confirm in game which values of `r_citadel_selection_outline2_width` and `cl_glow_brightness` actually change the outline, then tighten the slider ranges.
- Check the console log after launch for errors about cheat-protected convars coming from `autoexec.cfg`.

## Notes / gotchas
- Saved user values in `user_convars*.vcfg` override the `gameinfo.gi` ConVars block. That is why the color goes in the vcfg files and in autoexec.
- `autoexec.cfg` is run on every launch (the console log shows `execing autoexec.cfg`).
- Deadlock Mod Manager manages the `SearchPaths` block in `gameinfo.gi`. With `autoResetGameinfoOnExit` off, it leaves the ConVars block alone.
- Running process names are `project8.exe` and `deadlock.exe`.
- `.bak` files are made once and never overwritten. They hold the originals for "Restore originals".
