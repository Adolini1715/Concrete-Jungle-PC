PREDATOR: CONCRETE JUNGLE - PC PORT (Windows x64)
=================================================

A static recompilation of the 2005 Xbox game, rebuilt as a native Windows
program. It contains NO game data. You need your own copy of the game,
extracted from the disc (the folder with default.xbe and the other files).

Contents
  pcj.exe              the game
  SDL2.dll, libepoxy-0.dll, libwinpthread-1.dll   keep next to pcj.exe
  pcj.ini              settings (most are also in Options > PC Settings)
  textures\            put texture packs here (folders or .zip files)

FIRST RUN
  1. Put this folder anywhere, for example C:\Games\PCJ.
  2. Run pcj.exe. It asks for your game folder once and remembers it in
     pcj.ini (game=...). A folder named "game" next to pcj.exe is found
     on its own.
  3. Saves go to the "save" folder next to pcj.exe. It holds the emulated
     Xbox hard disk (partition images, about 5 GB on NTFS).

DEFAULT CONTROLS (keyboard and mouse)
  WASD move, mouse (or I J K L) look, Space jump, Shift lock on, F ranged
  attack, R context / light melee, Q heavy attack, E flip / ledge grab,
  Z gadget, C toggle cloak, Tab weapon select, 1 / 2 the stick clicks,
  arrows the d-pad, Esc pause.
  Every one of these can be changed: Options > Controller (or the pause
  menu's Controller tab) lists them all while you play on keyboard. Press
  Down past the page's last row to enter the list, pick an action, press
  Enter (or click it), then the new key or mouse button. Esc cancels; a
  key already in use swaps over. Mouse Look, Mouse Speed, Invert Mouse
  and Reset Keys are at the bottom of the list. Bindings are saved in
  pcj.ini as CJ_KEY_<button>=<key>.
  The menus work with the mouse: point, click, right click to go back,
  wheel to scroll. The button prompts on screen can be clicked, and on
  Hunting Gear a click sends the lasers to a slot (click again to choose).

KEYS
  F5 / F8        quick save / quick load
  F3             texture packs on / off (compare with the original)
  F7             frame rate 30 / 60 (also Options > PC Settings)
  F11, Alt+Enter fullscreen
  F10            free the mouse cursor (mouse look)
  F12            screenshot (saved in the screenshots folder)
  Esc            pause (in play) / back (in menus)
  Enter          Start / select
  Tab            weapon select
  Keyboard and controller prompts switch to whatever you used last.

SETTINGS
  Options > PC Settings in the game: widescreen, fullscreen, v-sync,
  60 fps, resolution scale (up to 5x, for 4K), mouse look, modern camera, PC
  button prompts, texture packs. They are saved to pcj.ini.

TEXTURE PACKS
  Drop a pack folder or .zip into textures\. Packs load in name order;
  a later pack wins where two replace the same texture. F3 turns them
  off and on while you play.

PROBLEMS
  Everything the game prints goes to pcj-log.txt next to pcj.exe.
  Include that file when you report a problem.
