# Toronja changelog

This file is compiled into Toronja: the What's New card and Settings > General > Release
notes show it, with no network needed. Newest first. Each section starts with a line
"## Toronja <version>"; keep that form, the app looks for it.

## Toronja 0.9.41

- Texture packs: fewer stops when an area is entered. The pack's pictures were decoded on the main thread's core at its priority; they are now decoded on the spare cores (Switch 2) and at low priority. A frame copies at most 24 MiB of pictures to the GPU, so they appear one after another instead of holding the frame.
- Switch 2: a game starts at once again, and the shaders it is known to need are built while it runs. A count in the bottom left corner ("Building shaders 120/800") shows how far that is. Switch 1 still waits before the game, where it has one core to build with. Display > Shaders > Build Shaders Before Starting chooses; the setting starts from its new default on both consoles.

## Toronja 0.9.40

- Texture packs no longer run the emulator out of memory. Every picture of the pack that had been shown stayed in memory for the whole session; hours into The Legend of Zelda: Majora's Mask 3D with a 4K pack there was none left, and Toronja closed. Pictures are now freed some time after they are uploaded, and when memory is short new ones wait and the game's own textures are shown instead.
- Texture packs: a picture that cannot be loaded is left out. It used to be uploaded empty, which could close the emulator.

## Toronja 0.9.39

- Texture packs: a texture the pack had already replaced could get the game's own pixels written into it, or a replacement of another size copied over it, when the game put a different texture at the same place (item icons do). That is fixed. It is the likely cause of a freeze seen two hours into The Legend of Zelda: Majora's Mask 3D with a 4K pack, not a proven one.
- Texture packs: the log says how much memory the pack's textures use, once a minute while it changes, and when the limit is reached.

## Toronja 0.9.38

- Shaders are built before the game starts. With async shader compilation on, a game used to start at once with everything its shader cache listed still to build: a slow first minute or two with textures missing. That work is now done while the game loads, with a count on screen, and B skips the wait. It only takes long the first time a game is started with nothing compiled; after that it passes at once. Display > Shaders > Build Shaders Before Starting turns it off.

## Toronja 0.9.37

- The author line the Homebrew Menu shows reads "Olmectron (based on Azahar, Dekopon)", so it is clear whose work Toronja builds on.

## Toronja 0.9.36

- Switch 1: shorter freezes while a game's shaders are built for the first time. Pipelines built by the frame that needs them were meant to take a quarter of the time and took all of it: Super Mario 3D Land froze for up to 13 seconds at a time.

## Toronja 0.9.35

- The release notes are inside the app now. The What's New card and the release notes in the settings used to be fetched from GitHub and showed nothing on a console that is not online.
- After an update, What's New lists every version since the one that was installed, not only the newest.

## Toronja 0.9.34

- The library remembers the last game. It opens with the game that was started last already selected, when Toronja starts and when coming back from a game.
- What is on screen is compiled first. After a driver update every shader pipeline of a game is built again when the game starts, which can take minutes, and anything needed on screen waited at the back of that queue with its draws left out: Kirby: Planet Robobot showed its area map without a background. A draw now sends its pipeline to the head of the queue.

## Toronja 0.9.32

- Switch 2: games that use geometry shaders no longer close the emulator. Pokemon Omega Ruby closed Toronja at its title screen.
- Switch 2: built with OunceVK 20261118. The first start of each game after this update rebuilds its shaders: expect a slow minute or two, once per game.
- A diagnostic trace for crashes that leave no report: with an empty file log/diag.txt in Toronja's folder, every log line is written to the card at once. Slow; delete the file afterwards.

## Toronja 0.9.28

- First release of Toronja, for Switch 2 and Switch 1 from the same source, and of Toronja Link.
- Toronja Link: a second console shows one of the 3DS screens or both, sends its controls back and can play the sound, over direct wireless or the local network.
- The library as a shelf of game cases, with covers and each game's animated 3D banner and jingle.
- Menus redesigned: options in pages with an explanation for every setting, a new in-game menu.
- Freezes of 15 to 30 seconds in long sessions are gone.

## Known issues

- While a shader is built for the first time, the draws that need it are left out. Something a game draws only once can stay missing until that screen is opened again.
- The frame rate dips for a moment the first time a place is entered.
- On Switch 1, start Toronja over a game (title takeover), not from the Album.
