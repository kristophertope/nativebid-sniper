import base64
import os
import re
import struct
import sys


def read_tlv(der, pos):
    tag = der[pos]
    length = der[pos + 1]
    pos += 2
    if length & 0x80:
        count = length & 0x7F
        length = int.from_bytes(der[pos:pos + count], "big")
        pos += count
    return tag, pos, pos + length


def subject_and_key(der):
    _, start, _ = read_tlv(der, 0)
    _, tbs, _ = read_tlv(der, start)
    pos = tbs
    tag, _, end = read_tlv(der, pos)
    if tag == 0xA0:
        pos = end
    fields = []
    while len(fields) < 6:
        tag, body, end = read_tlv(der, pos)
        fields.append(der[pos:end])
        pos = end
    return fields[4], fields[5]


def build(pem_path, out_path):
    pem = open(pem_path).read()
    blocks = re.findall(r"-----BEGIN CERTIFICATE-----(.+?)-----END CERTIFICATE-----", pem, re.S)
    entries = {}
    for block in blocks:
        der = base64.b64decode("".join(block.split()))
        name, key = subject_and_key(der)
        entries[name] = key
    out = bytearray(struct.pack(">H", len(entries)))
    for name in sorted(entries):
        key = entries[name]
        out += struct.pack(">HH", len(name), len(key)) + name + key
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(out)
    return len(entries), len(out)


if __name__ == "__main__":
    count, size = build(sys.argv[1], sys.argv[2])
    print(f"{count} root certificates, {size} bytes")
else:
    Import("env")
    project = env.subst("$PROJECT_DIR")
    target = os.path.join(project, "data", "cert", "x509_crt_bundle.bin")
    if not os.path.exists(target):
        count, size = build("/etc/ssl/cert.pem", target)
        print(f"Built certificate bundle: {count} roots, {size} bytes")
