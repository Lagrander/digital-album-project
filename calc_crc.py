import sys

def crc8(data):
    crc = 0xFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0x31) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc

data = bytes.fromhex("18 6A CD 00 5C 89")
print(f"Example calc: {hex(crc8(data))} (expected 91)")
