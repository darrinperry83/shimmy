#!/usr/bin/env python3
"""Read the active stock IMG4 kernel collection; extract only into local out/."""
import ctypes
import hashlib
import json
from pathlib import Path
import struct
import uuid
from stock_inventory import OUT, im4p_object, der_node, file_hash


def load_commands(data, header=0):
    if struct.unpack_from("<I", data, header)[0] != 0xfeedfacf:
        raise ValueError("expected thin 64-bit Mach-O")
    ncmds, command_bytes = struct.unpack_from("<II", data, header + 16)
    position = header + 32
    limit = position + command_bytes
    if limit > len(data):
        raise ValueError("truncated Mach-O commands")
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, position)
        if size < 8 or position + size > limit:
            raise ValueError("malformed Mach-O command")
        yield cmd, position, size
        position += size


def image_uuid(data, header):
    for cmd, offset, size in load_commands(data, header):
        if cmd == 0x1b and size == 24:
            return str(uuid.UUID(bytes=data[offset + 8:offset + 24])).upper()
    return None


def fileset_entries(data):
    if struct.unpack_from("<I", data, 12)[0] != 0xc:
        raise ValueError("expected MH_FILESET")
    result = []
    for cmd, position, size in load_commands(data):
        if cmd != 0x80000035:
            continue
        if size < 32:
            raise ValueError("short fileset entry")
        file_offset = struct.unpack_from("<Q", data, position + 16)[0]
        name_offset = struct.unpack_from("<I", data, position + 24)[0]
        if not 32 <= name_offset < size:
            raise ValueError("invalid fileset name")
        name = data[position + name_offset:position + size].split(b"\0", 1)[0].decode()
        result.append({"identifier": name, "uuid": image_uuid(data, file_offset)})
    if not any(x["identifier"] == "com.apple.kernel" for x in result):
        raise ValueError("no kernel in fileset")
    if len({x["identifier"] for x in result}) != len(result):
        raise ValueError("duplicate fileset entries")
    return result


def decode_collection(data):
    """Decode an IMG4 collection, or validate an already decoded fileset."""
    if data[:4] == b"\xcf\xfa\xed\xfe":
        fileset_entries(data)
        return data
    image = im4p_object(data)
    _, position, limit = der_node(image, 0, len(image))
    fields = []
    while position < limit:
        tag, start, end = der_node(image, position, limit)
        fields.append((tag, image[start:end]))
        position = end
    if len(fields) < 4 or fields[1] != (0x16, b"krnl") or fields[3][0] != 4:
        raise ValueError("expected a kernel IM4P payload")
    payload = fields[3][1]
    if not payload.startswith((b"bvx2", b"bvx1", b"bvx-")):
        raise ValueError("unsupported kernel compression format")
    compression = ctypes.CDLL("/usr/lib/libcompression.dylib")
    decode = compression.compression_decode_buffer
    decode.restype = ctypes.c_size_t
    decode.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                       ctypes.c_size_t, ctypes.c_void_p, ctypes.c_int]
    capacity = 256 * 1024 * 1024
    output = ctypes.create_string_buffer(capacity)
    packed = ctypes.create_string_buffer(payload)
    size = decode(output, capacity, packed, len(payload), None, 0x801)  # COMPRESSION_LZFSE
    if not 0 < size < capacity:
        raise ValueError("kernel decompression failed or exceeds bound")
    decoded = output.raw[:size]
    fileset_entries(decoded)  # validate before writing
    return decoded


def main():
    inventory = json.loads((OUT / "driver-check/stock-inventory.json").read_text())
    source = Path(inventory["boot_objects"]) / "System/Library/Caches/com.apple.kernelcaches/kernelcache"
    target = OUT / "driver-check/stock-boot.kc"
    receipt_path = OUT / "driver-check/stock-collection.json"
    source_digest = file_hash(source)
    cache_valid = False
    if receipt_path.exists() and target.exists():
        old = json.loads(receipt_path.read_text())
        cache_valid = old["source_sha256"] == source_digest and old["extracted_sha256"] == file_hash(target)
    if not cache_valid:
        decoded = decode_collection(source.read_bytes())
        target.write_bytes(decoded)
    entries = fileset_entries(target.read_bytes())
    by_id = {x["identifier"]: x for x in entries}
    missing, mismatched = [], []
    for item in inventory["loaded_drivers"]:
        identifier = item["identifier"]
        if identifier.startswith("com.apple.kpi."):
            continue
        if identifier not in by_id:
            missing.append(identifier)
        elif item["uuid"] != by_id[identifier]["uuid"]:
            mismatched.append(identifier)
    report = {"source": str(source), "source_sha256": source_digest,
              "extracted": str(target), "extracted_sha256": file_hash(target),
              "product_build": inventory["product_build"], "board": inventory["board"],
              "entries": entries, "loaded_ids_absent_from_boot_collection": missing,
              "loaded_uuid_mismatches": mismatched,
              "signature_verification_performed": False, "custom_collection_linked": False}
    receipt_path.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Active stock collection: {len(entries)} fileset entries; "
          f"{len(missing)} loaded IDs outside boot collection; {len(mismatched)} UUID mismatches")
    if mismatched:
        raise SystemExit("stock collection does not match loaded driver UUIDs")


if __name__ == "__main__":
    main()
