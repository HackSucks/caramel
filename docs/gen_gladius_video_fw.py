from pathlib import Path
import hashlib
import struct
import sys
import zlib


IMAGES = (
    ('gladius_uvd.bin', 348508,
     '50152ab730df76b956238dcfd882d821acae1b9b0cf9a474b81d64db98c119cf',
     5, 0x01009832),
    ('gladius_vce.bin', 150112,
     'e48f8da73846a5f49f262db4a8cdd0d8a35a6e700a71df55cbd333e2cf9d4579',
     1, 0),
)


def main():
    if len(sys.argv) != 4:
        raise SystemExit('usage: gen_gladius_video_fw.py UVD_RAW VCE_B_RAW OUTPUT_DIR')
    destination = Path(sys.argv[3])
    destination.mkdir(parents=True, exist_ok=True)
    for source, (name, size, digest, ip, version) in zip(sys.argv[1:3], IMAGES):
        payload = Path(source).read_bytes()
        if len(payload) != size or hashlib.sha256(payload).hexdigest() != digest:
            raise SystemExit('unexpected firmware image: ' + str(source))
        header = bytearray(0x100)
        struct.pack_into('<IIHHHHIIII', header, 0,
                         size + 0x100, 0x20, 1, 0, ip, 0,
                         version, size, 0x100, zlib.crc32(payload))
        target = destination / name
        target.write_bytes(header + payload)
        print(name, len(header) + size,
              hashlib.sha256(target.read_bytes()).hexdigest())


if __name__ == '__main__':
    main()
