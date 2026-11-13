# deadlock-outlines

win32 + gdi+ settings app for deadlock outlines/glow/enemy color. one file: `src/main.cpp`.

- build: `build.cmd` (finds msvc via vswhere), output `build\DeadlockOutlines.exe`
- test on copies, never the real game files: `build\DeadlockOutlines.exe --root <folder>` (folder with gameinfo.gi, autoexec.cfg, *.vcfg)
- he likes the look as is, don't restyle the ui unless asked
- release: push a `v*` tag, ci attaches the exe
- desktop copy lives at `%USERPROFILE%\Desktop\DeadlockOutlines.exe`, refresh it after rebuilding
