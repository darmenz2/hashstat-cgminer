#!/usr/bin/env python3

import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import struct
from differential_work import Job, Result, Bytes, array, sha2


class Reply(C.Structure):
    _fields_ = [("status", C.c_int), ("nonce", C.c_uint32),
                ("internal_nonce", C.c_uint32), ("version_bits", C.c_uint32),
                ("internal_version_bits", C.c_uint32)] + [
                (name, C.c_uint8) for name in ("slot", "chip", "core", "opaque4", "tail")]


def main():
    parser = argparse.ArgumentParser(description=None)
    parser.add_argument("--build", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    build = args.build
    checks = 0

    def check(condition, message):
        nonlocal checks
        checks += 1
        if not condition:
            raise ValueError(message)

    check(build.is_absolute() and build.is_dir() and not build.is_symlink(), "absolute build required")
    output = build / "captured-work-oracle.json"
    check(not output.exists() and not output.is_symlink(), "new output required")
    receipt = json.loads((build / "build-receipt.json").read_text())
    check(receipt["status"] == "MODULE_BUILD_AND_NATIVE_TESTS_PASSED", "build failed")
    for name, expected in receipt["sourceSHA256"].items():
        rel = Path(name)
        check(not rel.is_absolute() and ".." not in rel.parts, "invalid input path")
        check(hashlib.sha256((root / rel).read_bytes()).hexdigest() == expected, "source mismatch: " + name)
    candidates = [build / name for name in ("libhs-reconstruction.dylib", "libhs-reconstruction.so") if (build / name).exists()]
    check(len(candidates) == 1, "one trusted library required")
    library = candidates[0]
    check(library.is_file() and not library.is_symlink(), "regular library required")
    library_sha = hashlib.sha256(library.read_bytes()).hexdigest()
    check(receipt["outputSHA256"][library.name] == library_sha, "library differs from build receipt")
    lib = C.CDLL(str(library))
    lib.hs_bm1362_rx_decode_long.argtypes = [C.POINTER(C.c_uint8), C.c_size_t]
    lib.hs_bm1362_rx_decode_long.restype = Reply
    lib.hs_work_build.argtypes = [C.POINTER(Job), C.POINTER(C.c_uint8), C.c_size_t, C.POINTER(Result)]
    lib.hs_work_build.restype = C.c_int
    fixture_path = root / "tests/fixtures/captured-work.json"
    fixture = json.loads(fixture_path.read_text())
    rx = bytes.fromhex(fixture["rx"])
    reply = lib.hs_bm1362_rx_decode_long(array(rx), len(rx))
    check(reply.status == 1, "shared-fields decoder failed")
    check(reply.nonce == int(fixture["expectedSubmitNonce"], 16), "canonical nonce")
    check(reply.internal_nonce == int.from_bytes(rx[2:6], "big"), "ASIC coordinate nonce")
    check(reply.version_bits == int(fixture["expectedSubmitVersionBits"], 16), "canonical version bits")
    p = fixture["notify"]
    mask = int(fixture["versionMask"], 16)
    check(reply.version_bits & ~mask == 0, "rolling outside negotiated mask")
    version = (int(p[5], 16) & ~mask) | reply.version_bits
    pieces = [bytes.fromhex(x) for x in (p[2], fixture["extranonce1"], fixture["extranonce2"], p[3])]
    held = [array(x) for x in pieces]
    branches = array(b"".join(bytes.fromhex(x) for x in p[4]))
    prev_stratum = bytes.fromhex(p[1])
    prev = b"".join(prev_stratum[n:n + 4][::-1] for n in range(0, 32, 4))
    ntime, bits = int(p[7], 16), int(p[6], 16)
    job = Job(*(Bytes(b, len(x)) for b, x in zip(held, pieces)), branches,
              len(p[4]), (C.c_uint8 * 32).from_buffer_copy(prev),
              version, ntime, bits, reply.nonce)
    scratch = array(bytes(sum(map(len, pieces))))
    result = Result()
    check(lib.hs_work_build(C.byref(job), scratch, len(scratch), C.byref(result)) == 0, "work build")
    coinbase = b"".join(pieces)
    merkle = sha2(coinbase)
    check(bytes(result.coinbase) == merkle, "coinbase hash")
    for branch in p[4]:
        merkle = sha2(merkle + bytes.fromhex(branch))
    expected = struct.pack("<I", version) + prev + merkle + struct.pack("<III", ntime, bits, reply.nonce)
    check(bytes(result.header) == expected == bytes.fromhex(fixture["expectedHeader"]), "full captured header")
    check(bytes(result.merkle) == merkle, "merkle root")
    check(bytes(scratch) == coinbase, "coinbase assembly")
    digest = sha2(expected)
    check(digest.hex() == fixture["expectedDigestLE"], "independent SHA256d")
    target = (0xffff << 208) // fixture["shareDifficulty"]
    check(int.from_bytes(digest, "little") <= target, "captured share target")
    wrong = sha2(expected[:76] + expected[76:][::-1])
    check(int.from_bytes(wrong, "little") > target, "old endian error must fail PoW")
    report = {"status": "PASS", "checks": checks, "fixtureSource": fixture["source"],
              "librarySHA256": library_sha, "fixtureSHA256": hashlib.sha256(fixture_path.read_bytes()).hexdigest(),
              "scriptSHA256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "header": expected.hex(), "digestLE": digest.hex(), "targetLE": target.to_bytes(32, "little").hex(),
              "nonce": f"{reply.nonce:08x}", "wrongInternalNonceRejected": True,
              "hardwareTestPerformed": False, "poolConnectionMade": False, "completeCgminer": False}
    with output.open("x") as handle:
        handle.write(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
