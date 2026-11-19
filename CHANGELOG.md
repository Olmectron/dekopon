# Toronja changelog

This file is compiled into Toronja: the What's New card and Settings > General > Release
notes show it, with no network needed. Newest first. Each section starts with a line
"## Toronja <version>"; keep that form, the app looks for it.

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
