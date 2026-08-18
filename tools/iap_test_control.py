#!/usr/bin/env python3
"""Create and decode the STM32F407 RAM test-control block."""

from __future__ import annotations

import argparse
import pathlib
import struct


MAGIC = 0x49545043
VERSION = 2
CONTROL = struct.Struct("<16I")

COMMANDS = {
    "none": 0,
    "install-b": 1,
    "hold-pending": 2,
    "confirm": 3,
    "fail": 4,
    "clear": 5,
    "test-read-failure": 6,
    "test-write-failure": 7,
    "activate-other": 8,
    "test-non-pending-confirm": 9,
    "test-boundaries": 10,
    "power-install": 11,
    "power-confirm": 12,
    "power-pause-on-boot": 13,
}

POWER_PHASES = {
    "erase": 1,
    "header": 2,
    "payload-25": 3,
    "payload-50": 4,
    "payload-90": 5,
    "before-pending": 6,
    "pending-control": 7,
    "first-boot": 8,
    "confirm-control": 9,
}

STATUSES = {
    0x000: "reset",
    0x100: "bootloader-start",
    0x101: "bootloader-jump-a",
    0x102: "bootloader-jump-b",
    0x1FF: "recovery",
    0x200: "app-a-running",
    0x201: "app-b-running",
    0x210: "installing-slot-b",
    0x211: "install-ok",
    0x212: "pending-held",
    0x213: "confirmed",
    0x214: "failing",
    0x215: "rolled-back",
    0x216: "read-failure-passed",
    0x217: "write-failure-passed",
    0x218: "switching-slot",
    0x219: "non-pending-confirm-passed",
    0x21A: "boundary-guards-passed",
    0x21B: "power-cut-armed",
}


def parse_int(value: str) -> int:
    return int(value, 0)


def make_control(command: int, argument0: int, argument1: int) -> bytes:
    words = [MAGIC, VERSION, argument0, argument1]
    words.extend([0] * 11)
    words.append(command)
    return CONTROL.pack(*words)


def decode_control(data: bytes) -> dict[str, int | str]:
    if len(data) < CONTROL.size:
        raise ValueError(f"control block must be at least {CONTROL.size} bytes")
    words = CONTROL.unpack_from(data)
    status = words[4]
    if status & 0xF0000000 == 0xE0000000:
        status_name = f"error({status & 0x0FFFFFFF})"
    else:
        status_name = STATUSES.get(status, "unknown")
    return {
        "magic": words[0],
        "version": words[1],
        "command": words[15],
        "argument0": words[2],
        "argument1": words[3],
        "status": words[4],
        "status_name": status_name,
        "detail": words[5],
        "boot_count": words[6],
        "last_slot": words[7],
        "last_boot_address": words[8],
        "reset_cause": words[9],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="action", required=True)

    make_parser = subparsers.add_parser("make")
    make_parser.add_argument("--command", choices=COMMANDS, required=True)
    make_parser.add_argument("--argument0", type=parse_int, default=0)
    make_parser.add_argument("--argument1", type=parse_int, default=0)
    make_parser.add_argument("--power-phase", choices=POWER_PHASES)
    make_parser.add_argument("--output", type=pathlib.Path, required=True)

    decode_parser = subparsers.add_parser("decode")
    decode_parser.add_argument("--input", type=pathlib.Path, required=True)

    args = parser.parse_args()
    if args.action == "make":
        if args.power_phase is not None and args.command != "power-install":
            make_parser.error("--power-phase requires --command power-install")
        argument1 = (
            POWER_PHASES[args.power_phase]
            if args.power_phase is not None
            else args.argument1
        )
        data = make_control(COMMANDS[args.command], args.argument0, argument1)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(data)
        print(f"wrote {len(data)} bytes to {args.output}")
        return 0

    decoded = decode_control(args.input.read_bytes())
    for key, value in decoded.items():
        if isinstance(value, int):
            print(f"{key:18}: 0x{value:08X} ({value})")
        else:
            print(f"{key:18}: {value}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
