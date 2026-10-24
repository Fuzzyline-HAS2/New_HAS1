import struct
import zlib

MAC = "AABBCCDDEEFF"
RING = "0123456789abcdef"
TOPIC = f"has1/blackbox/v1/{MAC}/batch"


def record(seq=1, kind=1, text=b"test", values=(1, -2, 3, -4)):
    raw = bytearray(64)
    struct.pack_into("<BBHIIIiiii", raw, 0, 0xB7, kind, 0, seq, 7, 12345, *values)
    raw[32:60] = text[:28].ljust(28, b"\0")
    struct.pack_into("<I", raw, 60, zlib.crc32(raw[:60]))
    return bytes(raw)


def payload(seqs=(1,), device="iotglove", batch="00000001", records=None):
    rows = records if records is not None else [record(seq) for seq in seqs]
    return (f"BB1 {device} {MAC} {RING} {batch}\n" +
            "".join(raw.hex() + "\n" for raw in rows)).encode("ascii")
