import hmac
import hashlib
import sys

def sign_firmware(binary_path, secret, output_path):
    with open(binary_path, "rb") as f:
        firmware = f.read()

    sig = hmac.new(secret.encode("utf-8"), firmware, hashlib.sha256).digest()

    with open(output_path, "wb") as f:
        f.write(sig)

    print(f"완료!")
    print(f"펌웨어: {binary_path} ({len(firmware)} bytes)")
    print(f"서명:   {output_path} (32 bytes)")

if __name__ == "__main__":
    if len(sys.argv) != 4:
        print("사용법: python sign_firmware.py <firmware.bin> <secret> <output.sig>")
        sys.exit(1)

    sign_firmware(sys.argv[1], sys.argv[2], sys.argv[3])
