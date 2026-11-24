# Hotfix v0.2.2

Two fixes and one new experimental option on top of v0.2.1.

## Fixes

- **Windows Defender flagging the updater.** Check for Updates could be blocked by Windows Defender
  as `Trojan:Script/Wacatac.H!ml`. This is a false positive: the updater copied itself into the
  Temp folder as `pcj-update.bat` and ran from there, which looks like what malware does. The
  updater no longer does that.
  - **Updating from v0.2.1 still uses the old updater, so Defender may flag it one last time.**
    If it does, either restore the file in Windows Security (Protection history, then Actions,
    then Allow), or download the v0.2.2 zip from the release page and extract it over your
    install folder, then run `Install.bat`. Your saves, settings and texture packs are kept
    either way. Updates after this one use the fixed updater.
- **Jumping (modern controls).** From [issue #6](https://github.com/Adolini1715/Concrete-Jungle-PC/issues/6):
  pressing a direction and jump together sometimes gave a tiny instant hop instead of starting
  the charge. It now always starts the charge (a quick tap still jumps right away).

## New experimental option

- **Jump buffer** (PC Settings > Experimental, off by default). A jump pressed in the air is no
  longer lost when you land: still holding it as you touch down goes straight into the charge
  (keeping your momentum when moving), and a tap just before landing jumps again right after.
  Holding jump from take-off does not count, so the higher jump never turns into an extra hop.
  Try it and tell us how it feels.

## Updating

From v0.2.1: open the launcher and choose **Update**, or run `Update.bat` (see the Defender
note above). Your saves, settings and texture packs are kept.
