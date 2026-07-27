#!/usr/bin/env python3
"""Bridge SvxLink PTY PTT commands to the SX1255 TCP control port."""

import os
import socket
import time
import errno


PTY_PATH = os.getenv("SVX_PTY", "/run/svxlink/ptt")
SX_HOST = os.getenv("SX_HOST", "127.0.0.1")
SX_PORT = int(os.getenv("SX_PORT", "17020"))
SX_SECRET = os.getenv("SX_SECRET", "mytoken")


def set_radio_mode(mode: str) -> None:
    command = f"{SX_SECRET} {mode}\n".encode("ascii")
    with socket.create_connection((SX_HOST, SX_PORT), timeout=2.0) as sock:
        sock.sendall(command)
        reply = sock.recv(64).decode("ascii", errors="replace").strip()
    if reply != "OK":
        raise RuntimeError(f"SX1255 rejected {mode}: {reply or 'no response'}")


def run() -> None:
    print(
        f"SVXLink PTT bridge: pty={PTY_PATH} "
        f"sx1255={SX_HOST}:{SX_PORT}",
        flush=True,
    )

    pty_file = None
    while True:
        try:
            if pty_file is None:
                pty_file = open(PTY_PATH, "rb", buffering=0)
                print(f"PTY opened: {PTY_PATH}", flush=True)

            command = pty_file.read(1)
            if not command:
                pty_file.close()
                pty_file = None
                time.sleep(0.2)
            elif command == b"T":
                set_radio_mode("DUP")
                print("PTT ON -> DUP", flush=True)
            elif command == b"R":
                set_radio_mode("RX")
                print("PTT OFF -> RX", flush=True)
        except (FileNotFoundError, OSError, RuntimeError) as error:
            if isinstance(error, FileNotFoundError):
                time.sleep(0.2)
                continue
            if isinstance(error, OSError) and error.errno == errno.EIO and pty_file is not None:
                time.sleep(0.2)
                continue
            print(f"Bridge error: {error}", flush=True)
            if pty_file is not None:
                pty_file.close()
                pty_file = None
            time.sleep(0.5)


if __name__ == "__main__":
    run()
