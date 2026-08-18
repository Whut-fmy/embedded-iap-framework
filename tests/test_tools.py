from __future__ import annotations

import importlib.util
import pathlib
import struct
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


def load_module(name: str, path: pathlib.Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


pack_image = load_module("pack_image", ROOT / "tools" / "pack_image.py")
test_control = load_module("iap_test_control", ROOT / "tools" / "iap_test_control.py")


class PackImageTests(unittest.TestCase):
    def test_builds_aligned_image_with_payload_crc(self) -> None:
        slot_address = 0x08020000
        header_size = 0x200
        boot_address = slot_address + header_size
        payload = bytearray(64)
        struct.pack_into("<II", payload, 0, 0x20008000, boot_address + 9)

        image, crc32, stack_pointer, reset_handler = pack_image.build_image(
            bytes(payload), slot_address, 0x40000, header_size
        )

        fields = pack_image.HEADER_STRUCT.unpack_from(image)
        self.assertEqual(fields[0], pack_image.IAP_IMAGE_MAGIC)
        self.assertEqual(fields[1], header_size)
        self.assertEqual(fields[2], len(payload))
        self.assertEqual(fields[3], crc32)
        self.assertEqual(image[header_size:], payload)
        self.assertEqual(stack_pointer, 0x20008000)
        self.assertEqual(reset_handler, boot_address + 9)

    def test_rejects_reset_handler_outside_payload(self) -> None:
        payload = struct.pack("<II", 0x20008000, 0x08030001) + bytes(56)
        with self.assertRaises(ValueError):
            pack_image.build_image(payload, 0x08020000, 0x40000, 0x200)


class TestControlTests(unittest.TestCase):
    def test_round_trips_install_command(self) -> None:
        data = test_control.make_control(test_control.COMMANDS["install-b"], 8124, 0)
        decoded = test_control.decode_control(data)
        self.assertEqual(decoded["magic"], test_control.MAGIC)
        self.assertEqual(decoded["command"], test_control.COMMANDS["install-b"])
        self.assertEqual(decoded["argument0"], 8124)
        self.assertEqual(decoded["status_name"], "reset")

    def test_power_phase_values_match_firmware_protocol(self) -> None:
        self.assertEqual(
            test_control.POWER_PHASES,
            {
                "erase": 1,
                "header": 2,
                "payload-25": 3,
                "payload-50": 4,
                "payload-90": 5,
                "before-pending": 6,
                "pending-control": 7,
                "first-boot": 8,
                "confirm-control": 9,
            },
        )

    def test_round_trips_power_install_command(self) -> None:
        data = test_control.make_control(
            test_control.COMMANDS["power-install"],
            12416,
            test_control.POWER_PHASES["payload-50"],
        )
        decoded = test_control.decode_control(data)
        self.assertEqual(decoded["command"], 11)
        self.assertEqual(decoded["argument0"], 12416)
        self.assertEqual(decoded["argument1"], 4)


if __name__ == "__main__":
    unittest.main()
