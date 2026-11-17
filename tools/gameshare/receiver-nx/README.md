# Toronja Link

The GameShare receiver: shows on this console what Toronja is running on another one, and
sends the controls back. Builds for Switch 1 (libnx) and Switch 2 (libounce):

    make                    # Switch 1
    make PLATFORM=switch2   # Switch 2

On the console running the game: Options > GameShare > GameShare on (or the switch on the
first page of the in-game menu), choose the connection, start a game.

## Connections

- **Wi-Fi / LAN**: both consoles on the same network. The host is found by broadcast; if
  it is not, list its address in `sdmc:/switch/dekopon-link.txt`.
- **Local wireless**: console to console, no router.

## Local wireless and how the app is started

The system checks the network's *local communication id* against the NACP of the title the
app runs as:

- **From the Album's Homebrew Menu** (applet mode) there is no NACP to check, and joining
  works. This is the simple way.
- **On top of a game** (title override), the id is checked against that game's list, the
  host's id is not in it, and `ldnConnect` fails with `0xc2cb`. Using `ldn:s` does not
  change this.
- **As an installed forwarder** the app is a title of its own, so its NACP decides. A
  forwarder made for Toronja Link has to list the host's id among its
  `LocalCommunicationId` entries. The host's id is the one of the title its loader runs
  under; Toronja Link prints it while scanning (`found: id ...`) and writes it to
  `sdmc:/switch/dekopon-link.log`. On the Switch 2 used during development it was
  `0x0400486024DEC000`. Untested: nobody has built such a forwarder yet.

## Controls

- Hold **+ and -**, tap the tab in the corner, or swipe away from that corner: the menu
  (screens, sound, FPS, tab, close).
- Click **both sticks**: the other screen.
- Settings are kept in `sdmc:/switch/dekopon-link.ini`.
