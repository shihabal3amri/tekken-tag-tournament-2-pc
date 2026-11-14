# Playing from RecompLauncher

RecompLauncher installs a release from a `.zip` and runs the game from it. This game builds itself
from the player's disc, so the zip carries the installer in its silent mode:

    ttt2-<version>-windows-amd64.zip
        recomplauncher.json          (port/launcher/recomplauncher.json)
        ttt2_setup_probe.exe         (build_setup.cmd output: the asInvoker build, no admin prompt)

What the launcher does with it:

1. It asks the player for their disc image and Title Update 3.
2. It runs `ttt2_setup_probe.exe /silent <disc> <title-update> <install folder>`, which builds the
   game into the same folder (the same step the setup window runs).
3. Play starts `Tekken Tag Tournament 2.exe` from that folder.

Why `ttt2_setup_probe.exe` and not `ttt2_setup.exe`: `ttt2_setup.exe` asks for administrator rights
(Program Files, the Add or remove programs entry), and the launcher can't answer that prompt. The probe
build is the same program without it, so no uninstall entry is made; the launcher removes the game.

Name the zip with `windows` and `amd64`, like the server zips, so the launcher picks the right file.
The server zips are never taken for the game while a game zip is in the release.

Not tested end to end with a real disc image yet.
