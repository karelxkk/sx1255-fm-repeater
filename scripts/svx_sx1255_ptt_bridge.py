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

PA_PTT_ENABLED = os.getenv("PA_PTT_ENABLED", "0") == "1"
PA_PTT_CHIP = os.getenv("PA_PTT_CHIP", "/dev/gpiochip0")
PA_PTT_LINE = int(os.getenv("PA_PTT_LINE", "17"))
PA_PTT_ACTIVE_LEVEL = int(os.getenv("PA_PTT_ACTIVE_LEVEL", "1"))
PA_PTT_PRE_MS = float(os.getenv("PA_PTT_PRE_MS", "10"))
PA_PTT_POST_MS = float(os.getenv("PA_PTT_POST_MS", "10"))


class PaPtt:
    """Optional GPIO output used to key an external power amplifier."""

    def __init__(self) -> None:
        self.request = None
        self.line = PA_PTT_LINE
        if not PA_PTT_ENABLED:
            return

        try:
            import gpiod
            from gpiod.line import Direction, Value

            inactive = Value.INACTIVE if PA_PTT_ACTIVE_LEVEL == 1 else Value.ACTIVE
            self.request = gpiod.request_lines(
                PA_PTT_CHIP,
                config={
                    self.line: gpiod.LineSettings(
                        direction=Direction.OUTPUT,
                        output_value=inactive,
                    )
                },
                consumer="svxlink-pa-ptt",
            )
            print(
                f"PA PTT enabled: chip={PA_PTT_CHIP} line={self.line} "
                f"active_level={PA_PTT_ACTIVE_LEVEL}",
                flush=True,
            )
        except Exception as error:
            print(f"PA PTT init failed: {error}", flush=True)
            self.request = None

    def set_active(self, active: bool) -> None:
        if self.request is None:
            return

        from gpiod.line import Value

        if PA_PTT_ACTIVE_LEVEL == 1:
            value = Value.ACTIVE if active else Value.INACTIVE
        else:
            value = Value.INACTIVE if active else Value.ACTIVE

        try:
            self.request.set_value(self.line, value)
            print(f"PA PTT {'ON' if active else 'OFF'}", flush=True)
        except Exception as error:
            print(f"PA PTT set failed: {error}", flush=True)

    def close(self) -> None:
        if self.request is not None:
            self.request.release()
            self.request = None


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

    pa_ptt = PaPtt()

    pty_file = None
    try:
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
                    # Key the amplifier before switching the radio to TX.
                    pa_ptt.set_active(True)
                    time.sleep(PA_PTT_PRE_MS / 1000)
                    try:
                        set_radio_mode("DUP")
                    except RuntimeError:
                        pa_ptt.set_active(False)
                        raise
                    print("PTT ON -> DUP", flush=True)
                elif command == b"R":
                    # Switch back to RX first, then unkey the amplifier.
                    set_radio_mode("RX")
                    time.sleep(PA_PTT_POST_MS / 1000)
                    pa_ptt.set_active(False)
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
    finally:
        pa_ptt.close()


if __name__ == "__main__":
    run()
