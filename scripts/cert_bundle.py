# Generate ESP-IDF 4.4 compatible trust bundle from Mozilla roots (certifi).
# Format: big-endian count, then sorted (name length, key length, DER name, DER SPKI).
Import("env")
from pathlib import Path
import struct
import re
try:
    import certifi
    from cryptography import x509
    from cryptography.hazmat.primitives import serialization
except ImportError as exc:
    raise RuntimeError("Install certifi and cryptography in the PlatformIO Python environment; see README.") from exc

pem = Path(certifi.where()).read_bytes()
records = []
for block in re.findall(rb"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----", pem, re.S):
    cert = x509.load_pem_x509_certificate(block)
    name = cert.subject.public_bytes()
    key = cert.public_key().public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
    records.append((name, key))
records.sort(key=lambda r: r[0])
if not 1 <= len(records) <= 200:
    raise RuntimeError("Unexpected CA bundle size")
bundle = struct.pack(">H", len(records)) + b"".join(struct.pack(">HH", len(n), len(k)) + n + k for n, k in records)
target = Path(env.subst("$PROJECT_DIR")) / "include" / "generated_ca.h"
target.parent.mkdir(exist_ok=True)
lines = [",".join("0x%02x" % b for b in bundle[i:i+24]) for i in range(0,len(bundle),24)]
content = "#pragma once\n#include <stdint.h>\nstatic const uint8_t ARISTON_CA_BUNDLE[] = {\n" + ",\n".join(lines) + "\n};\n"
if not target.exists() or target.read_text() != content:
    target.write_text(content)
