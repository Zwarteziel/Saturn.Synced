Saturn.Synced 1.3 - high framerate mod for Hardwar (HardwarW.exe, Steam release)
================================================================================

DESCRIPTION
  Out of the box, Hardwar has a built-in frame limiter: after drawing a
  frame, it waits until 37 ms have passed before starting the next one.
  That way it never shows more than ~25-27 frames per second. The wait
  isn't needed to keep the game running correctly; it only limits how
  often a new frame is drawn. On a modern PC that pause is the only thing
  holding the game back, and 25 frames per second looks choppy.
  Saturn.Synced shortens that pause, so the game can show 60 or more frames
  per second. Flying, turning etc. look much smoother.
  The game itself does not run faster: Hardwar moves everything based on
  the time that has actually passed, not per frame. More frames simply
  means smaller steps.

  The mod also keeps time more precisely than the original game.
  The game counts in whole milliseconds, and at high frame rates the tiny
  leftovers would add up, making the game run a fraction too slow. The mod
  saves up those leftovers and uses them to stay in sync with real time.

  The mod only adjusts the game while it is running. Delete dinput8.dll
  and Hardwar is back to normal. The DLL checks the original bytes before
  patching anything. Multiplayer games are left untouched by default.


INSTALLATION
  1. Copy dinput8.dll and saturn_synced.ini into the Hardwar installation folder
     (the folder containing HardwarW.exe; for Steam usually
     ...\steamapps\common\Hardwar\).
     Updating from an earlier version: replace both files.

  2. Start the game as usual.

  3. Check saturn_synced.log in the same folder. It should contain:
        single-player limiter (37 ms) @00428ABF: patched


CHANGING THE INI
  All options are explained in saturn_synced.ini. Restart the game after
  saving it.


UNINSTALLING
  Delete dinput8.dll (and optionally saturn_synced.ini and the .log files).
  The game files themselves are never modified; all changes happen in
  memory only.


KNOWN RISKS / WHAT TO TEST
  - With shorter time steps per frame (e.g. 16 ms at 60 fps instead of
    37 ms), the fixed-point rounding in the physics *may* behave slightly
    differently. Let me know if things get messed up.

  - If you use software or a wrapper that enforces vsync, set TargetFPS to
    your monitor's refresh rate or a divisor of it (e.g. 72 or 144 on a
    144 Hz monitor). Without vsync, any value works; I use 60 fps.

  - A few antivirus engines that rely on machine-learning heuristics
    (including Microsoft Defender) may flag dinput8.dll as malware. This is
    a false positive: the DLL is a small proxy that patches the game's
    frame limiter in memory, which looks similar to techniques malware uses.
    The full source code is included in the src folder.


VERSION HISTORY
  v1.3 - Fixed more crashes related to deleted moths.
       - Added optional setting PreciseMovement for framerates >60 fps.
  v1.2 - Fixed crashes related to references to deleted moths or pilots.
  v1.1 - Fixed AI moths getting stuck in front of hangars
  v1.0 - First public release (as Saturn Synced).
