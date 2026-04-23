#!/usr/bin/env python3

import argparse
import glob
import sys
import time

import serial


DEFAULT_PROMPT = "dbg> "
DEFAULT_PATTERNS = (
    "/dev/cu.usbserial*",
    "/dev/cu.usbmodem*",
    "/dev/ttyUSB*",
    "/dev/ttyACM*",
)


def detect_port() -> str:
    matches = []
    for pattern in DEFAULT_PATTERNS:
        matches.extend(sorted(glob.glob(pattern)))

    if not matches:
        raise RuntimeError("no serial port detected; pass --port")
    if len(matches) > 1:
        joined = "\n  ".join(matches)
        raise RuntimeError(f"multiple serial ports detected; pass --port\n  {joined}")
    return matches[0]


def read_until_prompt(port: serial.Serial, prompt: str, timeout_s: float) -> str:
    deadline = time.monotonic() + timeout_s
    data = bytearray()
    prompt_bytes = prompt.encode("utf-8")

    while time.monotonic() < deadline:
        chunk = port.read(256)
        if chunk:
            data.extend(chunk)
            if prompt_bytes in data:
                break
            continue
        time.sleep(0.02)

    return data.decode("utf-8", errors="replace")


def strip_prompt(text: str, prompt: str) -> str:
    if text.endswith(prompt):
        return text[: -len(prompt)]
    return text


def run_commands(args: argparse.Namespace) -> int:
    port_name = args.port or detect_port()
    with serial.Serial(port_name, args.baud, timeout=0.1) as port:
        if not args.no_sync:
            read_until_prompt(port, args.prompt, args.timeout)

        for command in args.command:
            port.write(command.encode("utf-8") + b"\n")
            port.flush()
            response = read_until_prompt(port, args.prompt, args.timeout)
            sys.stdout.write(strip_prompt(response, args.prompt))
            if not response.endswith(args.prompt):
                sys.stderr.write(f"serial_debug.py: prompt not seen after command: {command}\n")
                return 1

    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Send one or more commands to the standalone ESP32 DDC debug shell.")
    parser.add_argument("command", nargs="+", help="command string(s) to send to the device")
    parser.add_argument("--port", help="serial port path; auto-detected when omitted")
    parser.add_argument("--baud", type=int, default=115200, help="serial baud rate")
    parser.add_argument("--timeout", type=float, default=8.0, help="seconds to wait for the debug prompt")
    parser.add_argument("--prompt", default=DEFAULT_PROMPT, help="debug shell prompt marker")
    parser.add_argument("--no-sync", action="store_true", help="skip initial read-until-prompt sync on connect")
    return parser.parse_args()


def main() -> int:
    try:
        return run_commands(parse_args())
    except RuntimeError as exc:
        sys.stderr.write(f"serial_debug.py: {exc}\n")
        return 2
    except serial.SerialException as exc:
        sys.stderr.write(f"serial_debug.py: {exc}\n")
        return 3


if __name__ == "__main__":
    raise SystemExit(main())