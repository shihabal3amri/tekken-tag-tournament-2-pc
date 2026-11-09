TEKKEN TAG TOURNAMENT 2 - native PC port
=========================================

This is a native Windows port of the Xbox 360 game, made by static recompilation. It ships no
game data: the installer built the game files on this PC from YOUR OWN copy of the game (the
Xbox 360 disc image plus Title Update 3). Do not share the installed "game" folder.

Windows 10 or 11, 64-bit. A DirectX 11 graphics card. Every runtime DLL that is not part of
Windows is already in this folder.


PLAYING
-------
Start "Tekken Tag Tournament 2" from the desktop or the Start menu.

An Xbox controller (or any XInput pad) is the intended way to play; whoever presses a button
first is player 1. The keyboard also works as player 1:

    Arrow keys      D-pad                 A / S / Z / X   A / B / X / Y buttons
    Enter or Space  START                 Q / W           left / right bumper
    Backspace       BACK

    Alt+Enter       windowed <-> borderless full screen

Window mode (windowed / borderless / full screen), the render resolution, VSYNC, BRIGHTNESS and
LOW LATENCY are in the game's own menus: OPTIONS > DISPLAY SETTINGS. EXIT on the main menu, or
closing the window, quits.
BRIGHTNESS (-5 to +5, 0 by default) makes the picture brighter or darker without washing out black
or white; it helps the darker stages when SHADOWS, BLOOM or LIGHT SHAFTS are off. DEFAULT on that
page puts VSYNC, BRIGHTNESS, LOW LATENCY and QUICK START back too; DISPLAY MODE and RESOLUTION stay as they are.
QUICK START (OFF by default): ON skips the logos and the intro movie when the game starts, straight to Press Start.
LOW LATENCY (OFF by default): ON shows each picture one frame (16.7 ms) sooner in fights. The work
that normally overlaps with the next frame then has to fit in the same one, so on a slower PC (a
dual-core processor, for example) fights can drop below 60 fps - leave it OFF there. AUTO turns it
on in fights only while the PC holds 60 fps with time to spare. Menus are always drawn the normal
way. ON and AUTO also let at most one finished picture wait for the graphics card (OFF: three).
FULLSCREEN takes over the screen while the game is the window in front. If it stays a borderless
window, click the game or Alt+Tab to it (the log names the program that was in front).
In BORDERLESS and FULLSCREEN the mouse pointer is hidden over the game. For the smoothest edges on
a strong graphics card, pick a RESOLUTION above your screen's: the picture is then scaled down to
the window.
VSYNC (ON by default) decides when a finished frame reaches the screen; the game runs at 60 frames
a second either way. OFF shows every frame at once: less input lag, but the picture may tear (not
on a G-SYNC / FreeSync display). It works in FULLSCREEN, and in BORDERLESS when Windows lets the
game flip the screen directly. In WINDOWED mode Windows always shows frames at the display's
refresh. Screen streaming or capture programs running in the background (game streaming hosts,
remote desktop tools, emulators with virtual displays, recorders) can stop Windows from handing
the game the screen: FULLSCREEN then stays a borderless window instead of switching back and
forth.
(The console's 3D MODE / 3D DEPTH rows are gone: stereoscopic 3D is not supported.)

OPTIONS > GRAPHICS SETTINGS has the picture options. Every row applies at once; DEFAULT puts them
all back.
  SHARPNESS (OFF, 1-10), ANTI-ALIASING and TEXTURE FILTERING: OFF / STANDARD by default, the
  game as it looks on the console. TEXTURE FILTERING offers TRILINEAR (smoother distant textures,
  very light) and ANISOTROPIC 2X-16X (sharp at an angle).
  ANTI-ALIASING: SMAA (light, keeps the picture sharp), or SSAA 1.5X / 2X - the game drawn at 1.5 / 2 times the RESOLUTION setting
  (at most 4 times 720p) and scaled down to the window. The smoothest edges, but much heavier on the GPU.
  SHADOWS, DEPTH OF FIELD (the blur of the stage behind the fighters), BLOOM (the glow around
  bright light) and LIGHT SHAFTS (rays of light on the stages that have them): ON by default.
  OFF is lighter on older graphics cards.
  WEATHER EFFECTS (ON by default): the falling rain and other particle effects some stages have.
  OFF saves processor time on slower PCs. It changes only the picture: online, each player can
  choose it for themselves.
  FPS COUNTER (OFF by default): the frames per second in a corner of the screen you choose.

Downloadable content: the bonus music packs (TEKKEN TUNES > TEKKEN MUSIC LIBRARY, switch
libraries with LB / RB) and bonus movie packs (GALLERY > TEKKEN THEATER) work when you have the
packages from your console. Choose their folder in the
installer's "Downloadable content" field. To add them to a game you already installed with the
same installer, leave the disc image and title update fields empty, choose the DLC folder and the
install folder, and press Install. Online, the other player does not need the same DLC.

Profiles: OPTIONS > SWITCH PROFILE opens the profile list; Y renames the highlighted profile (its
saves stay with it).

Friend Invite (Y in a Player Match room) lists your online Steam friends - those playing this
game first - and invites the one you pick into your room. They accept the invite in Steam and
join (when you play on Steam; on a community server, ask them to join the server instead).

Pair play (two players on one PC, one tag team) works offline and online. Online, choose PAIR at
character select, then press START on the second controller at "Press START button of controller
in use." The second controller plays as a guest of the first player's profile.

PAIR PLAY online (beta): 2v2 on four PCs, with the same screens as offline PAIR PLAY. Choose
ONLINE MODE > PAIR PLAY, then create or find a session (only PAIR PLAY sessions are listed; Player
Match stays 1v1). When everyone is in, the room host opens the room menu (Y) and picks TEAMS: on
the PAIR PLAY screen everyone presses START, moves to a stone (LEFT / RIGHT) and confirms with A
(each stone shows its player's name). Red and blue are the teams; the lower stone fights, the upper stone plays the second character
and tags in (three players make a 2v1). At character select the fighter picks first, then the
teammate picks the second character with their own controller. In the fight the teammate's
controller plays their character, tags included, and they watch it live on their own screen.
Both teammates play with the same input delay. Everyone in the session needs this version.


SAVES, SETTINGS AND THE LOG
---------------------------
Everything the game writes goes to

    %LOCALAPPDATA%\TTT2Recomp\

(saves and profiles, settings.ini, the shader cache, and log.txt). Uninstalling keeps that
folder unless you tick "Also delete my saved games". If something goes wrong, log.txt from
there is the file to send.

The installer prepared all the game's graphics shaders, so fights do not stutter the first time.
If they are ever missing (another Windows account, a deleted cache), the game builds them again
in the background by itself.


ONLINE (Steam)
--------------
Online Player Match and Ranked Match run over Steam: no ports to open, no Xbox LIVE.

  - The Steam client must be running and signed in on both PCs.
  - Both players need this port installed from their own copy of the game. Install from the
    same installer version on both PCs.
  - Host: ONLINE MODE > PLAYER MATCH > create a session. Steam shows you as playing
    "Spacewar" - that is Valve's public test app id this port uses for now; it is expected.
  - Join: ONLINE MODE > PLAYER MATCH > search. Hosts on Steam playing this port are listed.
  - Ranked: ONLINE MODE > RANKED MATCH > BEGIN SEARCH. On Steam you play with the ranks saved
    on your PC.

The first time ONLINE MODE is opened the game shows Namco's Online Mode terms; accept them to
continue. Online leaderboards, World Arena and voice chat are not available.

Online fights use rollback netcode. OPTIONS > GAME OPTIONS > ONLINE NETCODE sets the balance
between input delay and rollback:

  - STANDARD (default): the game's own input delay for your ping (3 frames on most connections);
    rollback only covers the ups and downs of the connection
  - PRIORITIZE RESPONSE: one frame less input delay, more rollback corrections; when the
    other player's inputs keep arriving late, the match takes STANDARD's delay by itself
  - PRIORITIZE GRAPHICS: one frame more input delay, fewer corrections
  - CUSTOM: set CUSTOM INPUT DELAY and CUSTOM ROLLBACK yourself

Input delay is how long your own button press is held before it is used, so it reaches the other
player in time. Rollback is what happens when the other player's input arrives later than that:
the game guesses, then corrects the picture. CUSTOM ROLLBACK is only the limit of how far it may
guess before it waits instead.

Both players always share one input delay: the host's ONLINE NETCODE decides it. CUSTOM
ROLLBACK (how many frames your game may correct before it waits) applies to your own game.
Rollback versions only play online with each other - not with 0.1.6 or older.


COMMUNITY SERVERS
-----------------
Besides Steam, the online modes can run on a community server (ttt2-server, hosted by players):
the server is where its players find each other - sessions, the game's news, profiles and
rankings - and the matches themselves still run directly between the two players over Steam
P2P, friends or not. ONLINE MODE > SERVER lists STEAM, your saved servers with their players
and ping, and servers on your local network:

  - A: play on the server (or on STEAM). The game remembers it and connects to it the next time
    you open ONLINE MODE; if it cannot, you play on Steam and the game tells you why.
  - ADD SERVER: type its address (name or IP, :port if not 28200; CTRL+V pastes).
  - Y: SERVER INFO - region, players, whose ranks count there, the host's message. X: REMOVE.

On a server you play only that server's players: its rooms are the only ones you see, and a
player who is not on it (on another server, or on plain Steam) cannot reach your game.
On a server, LEADERBOARDS show that server's players: Ranked results count when both players'
games report the same result; Survival and the other offline records are sent when you set
them (the game connects to your chosen server for that by itself). The server's news scroll in
the information bar at the bottom, between the game's tips.
Ranks: your ranks (each fighter's rank and points, Battle Points, your online record) are saved
on your PC. Each server decides whose ranks count there (SERVER INFO shows it):
  - kept by the server (the usual): everyone starts there as a Beginner and ranks up by playing
    ranked matches there. What you earn there stays on that server: the ranks saved on your PC
    do not change, and outside ONLINE MODE (PROFILE > RANK LIST) the game shows your own.
  - your own: you play there with the ranks saved on your PC, and its ranked matches change them.
As on Xbox LIVE, after a ranked match a dan-ranked winner's game sends the match's replay
(TEKKEN CHANNEL: search and download other players' replays, watch them in REPLAY THEATER) or
that player's ghosts (GHOST BATTLE then also fights other players' ghosts of your belt). TEAM
shows your team; the server's admin makes the teams.
On Steam, TEAM and LEADERBOARDS are grayed: they need a server. TEKKEN CHANNEL's
REPLAY THEATER (the replays saved on your PC) works on Steam too; its SEARCH REPLAYS needs a server.
WORLD ARENA is always grayed (no network has its lobbies).
How busy a server is: the list shows each server's players online and playing (in rooms and
matches); on a server, the SERVER row and the information bar show it too, updated every 10 s.
A server's admin can kick players (for a few minutes) and ban them (for some days or for good).
A server that kicked or banned you is left out of your server list until that ends, and the game
plays on Steam instead of trying it again. If the admin lifts it early, the server is back in your
list the next time you open it (every 2 minutes at most the game asks the server again), and
choosing a server always lets the server decide: it lets you in, or says why not.
Servers keep a history of the matches played on them.
A player's LICENSE (ranks, ranked record) is theirs: on a server your game gives the server your
license and shows other players' from there (after a ranked match too); on Steam it asks the other
player's game.

Servers know you by your Steam account, so Steam must be running. A server's password is saved
encrypted for your Windows account. The first connection remembers the server's key; if it ever
changes, the game refuses the server (remove it and add it again if its host replaced the key).


UNINSTALLING
------------
Settings > Apps > Installed apps > "Tekken Tag Tournament 2 (PC port)", or run uninstall.exe in
the install folder.


LEGAL
-----
Tekken Tag Tournament 2 is (c) Bandai Namco Entertainment. This port is an unofficial fan
project, not affiliated with or endorsed by Bandai Namco or Microsoft. You must own the game.
Steamworks: steam_api64.dll is (c) Valve Corporation, redistributed under the Steamworks SDK
licence.
