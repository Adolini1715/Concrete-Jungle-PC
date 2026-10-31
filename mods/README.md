# Mods (C++ source overlays on pcj-decomp)

Each `mods/<Unit>.cpp` replaces whole function definitions of pcj-decomp's
`src/*/<Unit>.cpp` for the PC build, without touching pcj-decomp (which stays
byte-matched to retail).

- List the functions in `tools/hybridize/targets.txt` as `mod=<Unit> <va> ...`.
  The original at that VA must be byte-matched (its layout, calls and register
  use are the reference). Changing the list needs the recompiler regen (RUN.md).
- In the mod file, `// MOD <va>` starts the replacement for that VA and runs to
  the next `// MOD` line. Text before the first `// MOD` is inserted after the
  unit's #includes (extra includes, `cjmod_` prototypes).
- Mods are written in the decomp's own C++ (same headers, names and types).
  `cjmod_*` functions are native helpers in src/native/hybrid/cjmod.c (scalar
  arguments only); use them for state or port features (settings, time).
- Mods run native by default. `CJ_MODS=0` turns all mods off (the recompiled
  original runs); `CJ_MODS=<va,va>` keeps only those.
