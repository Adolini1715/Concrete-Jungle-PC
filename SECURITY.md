# Security

## Reporting a problem

Please report security problems privately: use **Security > Report a vulnerability** on this
repository (GitHub private vulnerability reporting), not a public issue. Include what you
found, how to reproduce it, and the build you used.

## What to know as a player

- **There are no official prebuilt downloads of the game.** The port is built from your own
  copy with `build.sh`. Anyone offering a ready-made `pcj.exe` is not this project; do not run
  it.
- The only official sources are this repository, its Releases page and the pcj-decomp
  repository.
- **Texture packs and mods are data the game reads.** The game's own file loaders date from
  2005 and trust their files. Install packs and mods only from people you trust, as with any
  game mod. The port never loads code (DLLs, scripts) from packs, mods or saves.
- Save files from other people are read with checked sizes, but treat them like mods.

## What the build script does

`build.sh` installs Ubuntu packages with apt (asks for sudo), and downloads only:
xboxrecomp (pinned commit), pcj-decomp (pinned tag), SDL2 (pinned version, SHA-256 checked)
and libepoxy (pinned tag, commit checked). Read it before you run it.
