# Toronja Transfer

Moves files between a PC and the console's SD card over the local network, much faster than
an FTP server: large socket buffers, 2 MiB blocks, the file reserved at its final size, and
the network and the card each on a thread of their own. Builds for Switch 1 (libnx) and
Switch 2 (libounce):

    make                    # Switch 1
    make PLATFORM=switch2   # Switch 2

Open the app on the console; it shows its address and listens on TCP port 28302. On the PC
use `transferir.py` (kept with the user's LAN scripts, not in this repository). Startup steps
and errors go to `sdmc:/switch/toronja-transfer.log`.

## Protocol

Little-endian. One connection carries any number of requests, one after another.

Request: `"TTX1"`, `u8 op`, `u16 path length`, path (UTF-8, starts with `/`, relative to the
card's root, no `..`).

Reply: `u8 status`. 0 is success and the body follows; otherwise `u16 length` and a message.

| op | name   | request body | reply body |
|----|--------|--------------|------------|
| 0  | PING   | -            | `u64 free`, `u64 total`, `u16 length`, version text |
| 1  | LIST   | -            | `u32 count`, `u32 bytes`, then per entry `u8 is_dir`, `u64 size`, `u64 mtime`, `u16 length`, name |
| 2  | PUT    | `u64 size`   | status once the file is created; the client then sends `size` bytes; then status and `u32 crc32` |
| 3  | GET    | -            | `u64 size`, `size` bytes, `u32 crc32` |
| 4  | DELETE | -            | - (a file or an empty folder) |
| 5  | HASH   | -            | `u64 size`, `u32 crc32` (read back from the card) |
| 6  | MKDIR  | -            | - (with every folder on the way) |

PUT creates the folders on the way, writes to `<path>.ttpart` and renames it over the target
only when every byte arrived. The CRC-32 (zlib's) is computed on the console from the bytes
written to or read from the card. After an error in the middle of a transfer the server hangs
up.
