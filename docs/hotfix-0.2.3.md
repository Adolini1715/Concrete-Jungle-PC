# Hotfix v0.2.3

One fix on top of v0.2.2: antivirus false positives on the release download.

## Fix

- **Antivirus warnings on the download.** Some antivirus programs (Bitdefender and others that use
  its engine) flagged the release zip as `Gen:Heur.Bat.1`. The only file they reacted to was the
  old updater for v0.1 installs, `installer\Update-from-v0.1.bat`, which copied itself into the
  Temp folder before running. That file is gone. Nothing in the game or the installer changed.

## Coming from v0.1

v0.1 had no updater. Download `Concrete-Jungle-PC-v0.2.3.zip`, open it, drag everything in it
into the folder you unzipped v0.1 into, choose **Replace** for every file, and run `Install.bat`.
Your saves, settings and texture packs are kept.

## Updating

From v0.2.x: open the launcher and choose **Update**, or run `Update.bat`. Your saves, settings and
texture packs are kept. If you still have `installer\Update-from-v0.1.bat` from an older version,
you can delete it.
