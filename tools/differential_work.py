#!/usr/bin/env python3

import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import random
import struct


class Bytes(C.Structure):
    _fields_ = [("data", C.POINTER(C.c_uint8)), ("length", C.c_size_t)]


class Job(C.Structure):
    _fields_ = [(name, Bytes) for name in ("cb1", "ex1", "ex2", "cb2")] + [
        ("branches", C.POINTER(C.c_uint8)), ("branch_count", C.c_size_t),
        ("prev", C.c_uint8 * 32), ("version", C.c_uint32),
        ("ntime", C.c_uint32), ("nbits", C.c_uint32), ("nonce", C.c_uint32)]


class Result(C.Structure):
    _fields_ = [("header", C.c_uint8 * 80), ("coinbase", C.c_uint8 * 32),
                ("merkle", C.c_uint8 * 32), ("target", C.c_uint8 * 32)]


def require(ok, label):
    if not ok:
        raise AssertionError(label)


def array(data):
    return (C.c_uint8 * len(data)).from_buffer_copy(data)


def sha2(data):
    return hashlib.sha256(hashlib.sha256(data).digest()).digest()


def main():
    ap = argparse.ArgumentParser(description=None)
    ap.add_argument("--library", required=True, type=Path)
    ap.add_argument("--output", required=True, type=Path)
    args = ap.parse_args()
    require(args.library.is_absolute() and args.library.is_file() and not args.library.is_symlink(), "regular absolute library required")
    require(args.output.is_absolute() and args.output.parent.is_dir() and not args.output.exists() and not args.output.is_symlink(), "new absolute receipt required")
    raw_library = args.library.read_bytes()
    lib = C.CDLL(str(args.library))
    u8p = C.POINTER(C.c_uint8)
    lib.hs_work_build.argtypes = [C.POINTER(Job), u8p, C.c_size_t, C.POINTER(Result)]
    lib.hs_work_build.restype = C.c_int
    lib.hs_work_prevhash_from_stratum.argtypes = [u8p, C.c_size_t, u8p]
    lib.hs_work_prevhash_from_stratum.restype = C.c_int
    lib.hs_work_check_nonce.argtypes = [u8p, C.c_size_t, C.c_uint32, u8p, u8p, C.POINTER(C.c_int)]
    lib.hs_work_check_nonce.restype = C.c_int
    seed = 0x4841574F524B
    rng = random.Random(seed)
    cases = 2500
    checks = 0
    bits_values = (0x1D00FFFF, 0x207FFFFF, 0x1B0404CB, 0x03000001, 0x02008000, 0x2100FFFF)
    for case in range(cases):
        def data(n):
            return bytes(rng.getrandbits(8) for _ in range(n))
        parts = [data(rng.randrange(513)), data(rng.randrange(33)),
                 data(rng.randrange(33)), data(rng.randrange(513))]
        if not any(parts):
            parts[0] = b"\0"
        count = 32 if case % 100 == 0 else rng.randrange(9)
        branch_bytes = data(count * 32)
        prev_stratum = data(32)
        prev_wire = b"".join(prev_stratum[i:i + 4][::-1] for i in range(0, 32, 4))
        version, ntime, nonce = (rng.getrandbits(32) for _ in range(3))
        bits = bits_values[case % len(bits_values)]
        held = [array(part) for part in parts]
        branches = array(branch_bytes)
        job = Job(*(Bytes(b, len(part)) for b, part in zip(held, parts)),
                  branches, count, (C.c_uint8 * 32).from_buffer_copy(prev_wire),
                  version, ntime, bits, nonce)
        scratch = array(bytes([0xA5]) * (sum(map(len, parts)) + 9))
        result = Result()
        require(lib.hs_work_build(C.byref(job), scratch, len(scratch), C.byref(result)) == 0, (case, "build"))
        coinbase = b"".join(parts)
        root = sha2(coinbase)
        require(bytes(result.coinbase) == root, (case, "coinbase hash"))
        for offset in range(0, len(branch_bytes), 32):
            root = sha2(root + branch_bytes[offset:offset + 32])
        header = struct.pack("<I", version) + prev_wire + root + struct.pack("<III", ntime, bits, nonce)
        exponent, mantissa = bits >> 24, bits & 0x7FFFFF
        target = (mantissa << (8 * (exponent - 3))) if exponent >= 3 else mantissa >> (8 * (3 - exponent))
        require(bytes(result.header) == header, (case, "header"))
        require(bytes(result.merkle) == root, (case, "merkle"))
        require(bytes(result.target) == target.to_bytes(32, "little"), (case, "network target"))
        require(bytes(scratch) == coinbase + b"\xa5" * 9, (case, "scratch boundaries"))
        converted = array(bytes(32))
        require(lib.hs_work_prevhash_from_stratum(array(prev_stratum), 32, converted) == 0 and bytes(converted) == prev_wire, (case, "previous hash word order"))
        candidate_nonce = rng.getrandbits(32)
        expected_digest = sha2(header[:76] + struct.pack("<I", candidate_nonce))
        numeric_hash = int.from_bytes(expected_digest, "little")

        share_target = (max(1, numeric_hash - 1), numeric_hash,
                        min((1 << 256) - 1, numeric_hash + 1), rng.getrandbits(256) or 1)[case % 4]
        digest = array(bytes([0xCD]) * 32)
        accepted = C.c_int(-1)
        require(lib.hs_work_check_nonce(result.header, 80, candidate_nonce, array(share_target.to_bytes(32, "little")), digest, C.byref(accepted)) == 0, (case, "check nonce"))
        require(bytes(digest) == expected_digest and accepted.value == int(numeric_hash <= share_target), (case, "nonce/hash/target"))
        require(bytes(result.header) == header, (case, "header immutability"))
        checks += 10
    source_root = Path(__file__).resolve().parent.parent
    sources = ["include/hs_pow.h", "include/hs_work.h", "src/pow.c", "src/work.c", "tools/differential_work.py"]
    receipt = {"status": "PASS", "seed": seed, "cases": cases, "checks": checks,
               "oracle": "Python hashlib.sha256, struct LE serialization, arbitrary precision integer comparison",
               "librarySHA256": hashlib.sha256(raw_library).hexdigest(),
               "sourceSHA256": {name: hashlib.sha256((source_root / name).read_bytes()).hexdigest() for name in sources},
               "hardwareTestPerformed": False, "completeCgminer": False}
    with args.output.open("x") as output:
        output.write(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps({key: receipt[key] for key in ("status", "cases", "checks")}))


if __name__ == "__main__":
    main()
