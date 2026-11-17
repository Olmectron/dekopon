#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Downloads cover art on a PC and sends it to the console over FTP.

For a console that is kept off the internet: this machine fetches the covers from GameTDB
and uploads them to <user dir>/covers, where the library looks for them.

    python fetch_covers.py --host 192.168.0.100 --port 5000

It lists the games folder over FTP, reads each game's four-character code out of the ROM
header (a few bytes per file, nothing is downloaded whole), fetches the full wrap when
GameTDB has one and the front cover otherwise, and uploads what is missing.

Codes can also be given directly:  --codes AQEE AJRE
and the covers kept locally instead of uploaded:  --out covers  (no --host needed then).
"""
import argparse
import ftplib
import io
import os
import sys
import urllib.error
import urllib.request

SITE = "https://art.gametdb.com/3ds"
ROM_SUFFIXES = (".3ds", ".cci", ".cxi", ".app")


class Enough(Exception):
    pass


def read_range(ftp, path, offset, size):
    """Reads `size` bytes at `offset` of a remote file without fetching the rest."""
    data = bytearray()

    def take(block):
        data.extend(block)
        if len(data) >= size:
            raise Enough

    try:
        ftp.retrbinary("RETR " + path, take, blocksize=4096, rest=offset)
    except Enough:
        try:
            ftp.abort()
        except ftplib.all_errors:
            pass
        try:
            ftp.voidresp()
        except ftplib.all_errors:
            pass
    return bytes(data[:size])


def game_code(ftp, path):
    header = read_range(ftp, path, 0x100, 0x60)
    if header[:4] == b"NCCH":
        base = 0
    elif header[:4] == b"NCSD":
        base = int.from_bytes(header[0x20:0x24], "little") * 0x200
    else:
        return ""
    product = read_range(ftp, path, base + 0x150, 16).split(b"\0")[0].decode("ascii", "replace")
    code = product.rsplit("-", 1)[-1]
    return code if len(code) == 4 and code.isalnum() and code.isupper() else ""


def regions_for(code):
    last = code[-1]
    if last == "J":
        return ["JA", "US", "EN"]
    if last in "PXYZDFSI":
        return ["EN", "US", "JA"]
    if last == "K":
        return ["KO", "US", "EN"]
    return ["US", "EN", "JA"]


def download(code):
    """Returns (kind, bytes): kind is 'full' for a whole wrap, '' for a front cover."""
    for kind, folder in (("full", "coverfull"), ("", "cover")):
        for region in regions_for(code):
            url = f"{SITE}/{folder}/{region}/{code}.jpg"
            try:
                request = urllib.request.Request(url, headers={"User-Agent": "covers/1.0"})
                with urllib.request.urlopen(request, timeout=20) as response:
                    data = response.read()
                if len(data) > 1024:
                    return kind, data
            except (urllib.error.URLError, TimeoutError):
                continue
    return None, None


def remote_names(ftp, path):
    lines = []
    try:
        ftp.retrlines("LIST " + path, lines.append)
    except ftplib.all_errors:
        return []
    return [(line[0] == "d", line.split(None, 8)[-1]) for line in lines if line.strip()]


def ensure_dir(ftp, path):
    try:
        ftp.mkd(path)
    except ftplib.all_errors:
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--host", help="console's FTP address")
    parser.add_argument("--port", type=int, default=5000)
    parser.add_argument("--user-dir", default="/switch/dekopon", help="the app's folder on the SD card")
    parser.add_argument("--roms", help="games folder on the SD card (default: <user dir>/roms)")
    parser.add_argument("--codes", nargs="*", default=[], help="game codes to fetch, instead of scanning")
    parser.add_argument("--out", help="keep the covers in this local folder instead of uploading")
    args = parser.parse_args()
    if not args.host and not (args.codes and args.out):
        parser.error("give --host, or both --codes and --out")

    ftp = None
    codes = list(args.codes)
    if args.host:
        ftp = ftplib.FTP()
        ftp.connect(args.host, args.port, timeout=20)
        ftp.login()
        if not codes:
            roms = args.roms or args.user_dir + "/roms"
            for is_dir, name in remote_names(ftp, roms):
                if is_dir or not name.lower().endswith(ROM_SUFFIXES):
                    continue
                code = game_code(ftp, roms + "/" + name)
                print(f"{name}: {code or 'no game code'}")
                if code and code not in codes:
                    codes.append(code)

    covers = args.user_dir + "/covers"
    have = set()
    if ftp and not args.out:
        ensure_dir(ftp, covers)
        ensure_dir(ftp, covers + "/full")
        have = {name.rsplit(".", 1)[0] for _, name in remote_names(ftp, covers)}
        have |= {name.rsplit(".", 1)[0] for _, name in remote_names(ftp, covers + "/full")}

    for code in codes:
        if code in have:
            print(f"{code}: already on the console")
            continue
        kind, data = download(code)
        if data is None:
            print(f"{code}: GameTDB has no cover")
            continue
        label = "full wrap" if kind else "front"
        if args.out:
            folder = os.path.join(args.out, kind)
            os.makedirs(folder, exist_ok=True)
            with open(os.path.join(folder, code + ".jpg"), "wb") as file:
                file.write(data)
        else:
            target = f"{covers}/{'full/' if kind else ''}{code}.jpg"
            ftp.storbinary("STOR " + target, io.BytesIO(data))
        print(f"{code}: {label}, {len(data) // 1024} KB")
    if ftp:
        ftp.quit()
    return 0


if __name__ == "__main__":
    sys.exit(main())
