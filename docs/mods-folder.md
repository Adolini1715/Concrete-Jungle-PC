# Mods folder

A folder named `mods` next to pcj.exe (or `CJ_MODS_DIR=<folder>`; `CJ_MODS_DIR=0` turns it off)
holds files that replace the game's files. Paths inside it mirror the disc:

    mods/
      Predator/Binary/_bin_xb/Audio/_MUS_mfx_stealth.SFX     replaces a loose disc file
      Predator/Binary/_bin_xb/pred.edb                       replaces an archive member

The log names every replacement (`[MODDIR] ...`). Off in recordings (CJ_REC, CJ_REC_PLAY):
a replay has to read the same data it was recorded with.

## Two kinds of game file
- **Loose files** on the disc (audio banks in Audio/, .xmv movies, the bad-disc picture): the
  kernel's path translation opens the mods copy instead (xboxrecomp patch
  0023-path-overlay, `g_xbox_overlay_dir`). Any size.
- **Archive members**: everything listed in BuildData/Filelist.txt (the .edb packages: levels,
  characters, front end, and the audio banks too) is read out of Filelist.000-.005, a member
  often stored in two to four of them. src/mods_archive.c walks the game's own file lists in
  memory (EXFileSys::m_pFileSys) at each archive read (rebuilt when the lists change), finds
  the members that have a file in the mods folder, and patches every XBFileManager::FileRead
  (0x285180) that overlaps any copy of one with the mod file's bytes. A member's space is
  fixed, so the mod file can be **at most the member's size** (the `Len` in Filelist.txt);
  shorter is padded with zeros, longer is skipped with a log line.
- Check mode: `CJ_MODDIR_CHECK=1` logs each patched read and how many bytes the mod changed.

## Tools that help make mods
- tools/edb/edb_anim.py, anim_report.py: read the animation graph of an .edb.
- Texture replacement does not need an .edb at all: docs/texture-packs.md (textures/replace).
- Text: docs (text/replace.txt, src/textpack.c).

## Not yet
- Members that grow (would need the in-memory file list's length patched at load).
- New files the game does not already reference.
