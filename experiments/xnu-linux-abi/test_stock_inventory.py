#!/usr/bin/env python3
"""Check firmware parsing rejects malformed containers and detects payload changes."""
import hashlib
import json
from pathlib import Path
from stock_inventory import OUT, im4p_object, loaded_drivers


def tlv(tag, content):
    size = len(content)
    length = bytes([size]) if size < 128 else bytes([0x82]) + size.to_bytes(2, "big")
    return bytes([tag]) + length + content


payload = tlv(0x30, tlv(0x16, b"IM4P") + tlv(0x16, b"test") + tlv(4, b"payload"))
container = tlv(0x30, tlv(0x16, b"IMG4") + payload)
assert im4p_object(payload) == payload
assert im4p_object(container) == payload
for broken in (container[:-1], b"\x30\x80", b"\x30\x82\xff\xff", b"\x30\x03\x16\x01X", b"\x04\x00"):
    try:
        im4p_object(broken)
    except (ValueError, IndexError):
        pass
    else:
        raise AssertionError("malformed/non-Image4 container accepted")
snapshot = json.loads((OUT / "driver-check/stock-inventory.json").read_text())
entry = min(snapshot["active_fud_components"], key=lambda x: Path(x["active_file"]).stat().st_size)
original = Path(entry["active_file"]).read_bytes()
object_bytes = im4p_object(original)
assert hashlib.sha384(object_bytes).hexdigest() == entry["manifest_sha384"]
position = original.find(object_bytes)
assert position >= 0
modified = bytearray(original)
modified[position + len(object_bytes) - 1] ^= 1
assert hashlib.sha384(im4p_object(bytes(modified))).hexdigest() != entry["manifest_sha384"]
for text in ("", "not a driver list"):
    try:
        loaded_drivers(text)
    except ValueError:
        pass
    else:
        raise AssertionError("invalid stock driver list accepted")
print("STOCK_INVENTORY_CONTROLS_PASS: DER bounds/format, real firmware payload mutation, empty driver list")
