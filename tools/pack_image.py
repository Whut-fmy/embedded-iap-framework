#!/usr/bin/env python3
"""Build an embedded-iap-framework image from a slot-linked raw binary."""

from __future__ import annotations

import argparse
import binascii
import pathlib
import struct


IAP_IMAGE_MAGIC = 0x49415049
IAP_IMAGE_HEADER_VERSION = 1
HEADER_STRUCT = struct.Struct("<6I")


def parse_int(value: str) -> int:
    return int(value, 0)


def validate_vector_table(payload: bytes, boot_address: int) -> tuple[int, int]:
    if len(payload) < 8:
        raise ValueError("payload is too small to contain a Cortex-M vector table")

    stack_pointer, reset_handler = struct.unpack_from("<II", payload)
    stack_valid = (
        0x20000000 <= stack_pointer <= 0x20020000
        or 0x10000000 <= stack_pointer <= 0x10010000
    )
    if not stack_valid:
        raise ValueError(f"invalid initial stack pointer: 0x{stack_pointer:08X}")
    if reset_handler & 1 == 0:
        raise ValueError(f"reset handler is not a Thumb address: 0x{reset_handler:08X}")

    reset_address = reset_handler & ~1
    if not boot_address <= reset_address < boot_address + len(payload):
        raise ValueError(
            f"reset handler 0x{reset_handler:08X} is outside payload "
            f"0x{boot_address:08X}..0x{boot_address + len(payload):08X}"
        )
    return stack_pointer, reset_handler


def build_image(
    payload: bytes,
    slot_address: int,
    slot_size: int,
    header_size: int,
) -> tuple[bytes, int, int, int]:
    if header_size < HEADER_STRUCT.size or header_size % 4 != 0:
        raise ValueError("header size must fit the header and be 4-byte aligned")
    if header_size + len(payload) > slot_size:
        raise ValueError("packed image does not fit in the target slot")

    boot_address = slot_address + header_size
    stack_pointer, reset_handler = validate_vector_table(payload, boot_address)
    crc32 = binascii.crc32(payload) & 0xFFFFFFFF
    header = HEADER_STRUCT.pack(
        IAP_IMAGE_MAGIC,
        header_size,
        len(payload),
        crc32,
        IAP_IMAGE_HEADER_VERSION,
        0,
    )
    padding = bytes([0xFF]) * (header_size - len(header))
    return header + padding + payload, crc32, stack_pointer, reset_handler


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--slot-address", required=True, type=parse_int)
    parser.add_argument("--slot-size", default=0x40000, type=parse_int)
    parser.add_argument("--header-size", default=0x200, type=parse_int)
    args = parser.parse_args()

    payload = args.input.read_bytes()
    image, crc32, stack_pointer, reset_handler = build_image(
        payload,
        args.slot_address,
        args.slot_size,
        args.header_size,
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)

    print(f"input:         {args.input}")
    print(f"output:        {args.output}")
    print(f"payload size:  {len(payload)} bytes")
    print(f"packed size:   {len(image)} bytes")
    print(f"payload CRC32: 0x{crc32:08X}")
    print(f"initial MSP:   0x{stack_pointer:08X}")
    print(f"reset handler: 0x{reset_handler:08X}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
