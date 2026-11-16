#!/usr/bin/env python3
"""Test receiver for the Dekopon GameShare prototype (see src/citra_switch/game_share.h).

Shows the shared 3DS screen in a window and sends keyboard / mouse input back.

    pip install pillow
    python receiver.py <ip of the Switch 2> [--screen bottom|top] [--fps 30] [--quality 80]

Keys:  arrows = D-pad   X = A   Z = B   S = X   A = Y   Q = L   W = R
       Enter = Start    Right Shift = Select    I/J/K/L = circle pad
Mouse: click / drag on the picture = touch (only meaningful on the bottom screen)
"""
import argparse
import io
import socket
import struct
import time
import tkinter as tk

from PIL import Image, ImageTk

PORT = 28300
MAGIC = b"DKGS"
HELLO, INPUT, FRAME, WELCOME = 1, 2, 3, 4

# Bit positions follow SwitchFrontend::InputButton.
BUTTON = {"A": 0, "B": 1, "X": 2, "Y": 3, "Up": 4, "Down": 5, "Left": 6, "Right": 7,
          "L": 8, "R": 9, "Start": 10, "Select": 11}
KEYS = {"x": "A", "z": "B", "s": "X", "a": "Y", "Up": "Up", "Down": "Down", "Left": "Left",
        "Right": "Right", "q": "L", "w": "R", "Return": "Start", "Shift_R": "Select"}
STICK = {"i": (0, 1), "k": (0, -1), "j": (-1, 0), "l": (1, 0)}


class Receiver:
    def __init__(self, args):
        self.host = (args.host, PORT)
        self.screen = 1 if args.screen == "bottom" else 0
        self.quality, self.fps, self.scale = args.quality, args.fps, args.scale
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setblocking(False)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)

        self.width, self.height = (320, 240) if self.screen else (400, 240)
        self.root = tk.Tk()
        self.root.title("Dekopon GameShare - waiting for the host...")
        self.label = tk.Label(self.root, bg="black")
        self.label.pack()
        self.label.config(width=self.width * self.scale, height=self.height * self.scale)
        self.photo = None

        self.buttons, self.stick_keys = 0, set()
        self.touching, self.touch = False, (0, 0)
        self.seq = 0
        self.frame_id, self.chunks, self.chunk_count = -1, {}, 0
        self.frames, self.bytes, self.dropped, self.stat_time = 0, 0, 0, time.time()
        self.last_hello = 0.0

        self.root.bind("<KeyPress>", self.on_key)
        self.root.bind("<KeyRelease>", self.on_key)
        self.label.bind("<ButtonPress-1>", self.on_touch)
        self.label.bind("<B1-Motion>", self.on_touch)
        self.label.bind("<ButtonRelease-1>", self.on_release)
        self.root.after(1, self.tick)

    def on_key(self, event):
        down = event.type == tk.EventType.KeyPress
        key = event.keysym if len(event.keysym) > 1 else event.keysym.lower()
        if key in KEYS:
            bit = 1 << BUTTON[KEYS[key]]
            self.buttons = (self.buttons | bit) if down else (self.buttons & ~bit)
        elif key in STICK:
            (self.stick_keys.add if down else self.stick_keys.discard)(key)

    def on_touch(self, event):
        self.touching = True
        self.touch = (max(0, min(319, event.x // self.scale)), max(0, min(239, event.y // self.scale)))

    def on_release(self, _event):
        self.touching = False

    def send_input(self):
        x = sum(STICK[k][0] for k in self.stick_keys) * 32000
        y = sum(STICK[k][1] for k in self.stick_keys) * 32000
        self.seq += 1
        self.sock.sendto(MAGIC + struct.pack("<BIIhhBHH", INPUT, self.seq, self.buttons, x, y,
                                             1 if self.touching else 0, *self.touch), self.host)

    def show(self, jpeg):
        try:
            image = Image.open(io.BytesIO(jpeg))
            image.load()
        except Exception:
            self.dropped += 1
            return
        if self.scale != 1:
            image = image.resize((image.width * self.scale, image.height * self.scale), Image.NEAREST)
        self.photo = ImageTk.PhotoImage(image)
        self.label.config(image=self.photo, width=image.width, height=image.height)
        self.frames += 1
        self.bytes += len(jpeg)

    def receive(self):
        while True:
            try:
                data, _ = self.sock.recvfrom(2048)
            except BlockingIOError:
                return
            except ConnectionResetError:
                return
            if len(data) < 5 or data[:4] != MAGIC:
                continue
            if data[4] == FRAME and len(data) >= 22:
                frame_id, index, count, size, _w, _h, _screen = struct.unpack_from("<IHHIHHB", data, 5)
                if frame_id != self.frame_id:
                    if self.chunks and len(self.chunks) != self.chunk_count:
                        self.dropped += 1
                    self.frame_id, self.chunks, self.chunk_count = frame_id, {}, count
                self.chunks[index] = data[22:]
                if len(self.chunks) == count:
                    jpeg = b"".join(self.chunks[i] for i in range(count))
                    self.chunks = {}
                    if len(jpeg) == size:
                        self.show(jpeg)

    def tick(self):
        now = time.time()
        if now - self.last_hello >= 1.0:
            self.last_hello = now
            self.sock.sendto(MAGIC + struct.pack("<BBBB", HELLO, self.screen, self.quality, self.fps),
                             self.host)
        self.send_input()
        self.receive()
        if now - self.stat_time >= 2.0:
            seconds = now - self.stat_time
            self.root.title("Dekopon GameShare - %.1f fps, %d KB/s, %d dropped" %
                            (self.frames / seconds, self.bytes / seconds / 1024, self.dropped))
            self.frames = self.bytes = 0
            self.stat_time = now
        self.root.after(8, self.tick)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("host", help="IP address of the Switch 2 running Dekopon")
    parser.add_argument("--screen", choices=("bottom", "top"), default="bottom")
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--quality", type=int, default=80)
    parser.add_argument("--scale", type=int, default=2)
    Receiver(parser.parse_args()).root.mainloop()


if __name__ == "__main__":
    main()
