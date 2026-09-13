#!/usr/bin/env python3

import argparse
import ctypes as ct
import hashlib
import json
import os
from pathlib import Path
import random
import stat
import sys

OK, INVALID, TOO_LONG, ZERO, NEGATIVE, OVERFLOW, ABOVE_LIMIT = range(7)
MAX_INPUT = 16 * 1024 * 1024
MAX_U256 = (1 << 256) - 1
U8 = ct.c_uint8
U8P = ct.POINTER(U8)
SENTINEL = b"\xa5" * 32
GENESIS = bytes.fromhex(
    "01000000" + "00" * 32
    + "3ba3edfd7a7b12b27ac72c3e67768f617fc81bc3888a51323a9fb8aa4b1e5e4a"
    + "29ab5f49ffff001d1dac2b7c")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def file_hash(path):
    require(stat.S_ISREG(path.lstat().st_mode), "expected regular file: " + str(path))
    return hashlib.sha256(path.read_bytes()).hexdigest()


def as_buffer(value):
    return (U8 * max(1, len(value))).from_buffer_copy(value or b"\0")


def output_buffer():
    return as_buffer(SENTINEL)


def double_hash(value):
    return hashlib.sha256(hashlib.sha256(value).digest()).digest()


def decode_compact(bits):

    exponent = bits >> 24
    coefficient = bits & 0x7fffff
    magnitude = (coefficient >> (8 * (3 - exponent)) if exponent <= 3
                 else coefficient << (8 * (exponent - 3)))
    if magnitude == 0:
        return ZERO, None
    if bits & 0x800000:
        return NEGATIVE, None
    if magnitude > MAX_U256:
        return OVERFLOW, None
    return OK, magnitude


def encode_compact(magnitude):
    if magnitude == 0:
        return 0
    length = (magnitude.bit_length() + 7) // 8
    high = (magnitude << (8 * (3 - length)) if length <= 3
            else magnitude >> (8 * (length - 3)))
    if high & 0x800000:
        high >>= 8
        length += 1
    return (length << 24) | high


class Checks:
    def __init__(self):
        self.assertions = 0
        self.groups = {}

    def same(self, actual, expected, label):
        self.assertions += 1
        if actual != expected:
            raise AssertionError(f"{label}: actual={actual!r}; expected={expected!r}")

    def case(self, name):
        self.groups[name] = self.groups.get(name, 0) + 1


def load_library(path):
    library = ct.CDLL(str(path))
    for name in ("hs_sha256", "hs_sha256d", "hs_sha256d_header80"):
        function = getattr(library, name)
        function.argtypes = [U8P, ct.c_size_t, U8P]
        function.restype = ct.c_int
    library.hs_pow_target_from_compact.argtypes = [ct.c_uint32, U8P]
    library.hs_pow_target_from_compact.restype = ct.c_int
    library.hs_pow_compact_from_target.argtypes = [U8P, ct.POINTER(ct.c_uint32)]
    library.hs_pow_compact_from_target.restype = ct.c_int
    library.hs_pow_hash_meets_target.argtypes = [U8P, U8P, ct.POINTER(ct.c_int)]
    library.hs_pow_hash_meets_target.restype = ct.c_int
    library.hs_pow_check_compact.argtypes = [U8P, ct.c_uint32, U8P, ct.POINTER(ct.c_int)]
    library.hs_pow_check_compact.restype = ct.c_int
    return library


def run(library, count, seed, checks):
    rng = random.Random(seed)
    fixed_lengths = [0, 1, 2, 3, 7, 31, 32, 33, 55, 56, 57, 63, 64, 65,
                     79, 80, 81, 119, 120, 127, 128, 129, 255, 256, 511, 512,
                     1023, 1024, 4096, 65535]

    def check_hash(value, label):
        data = as_buffer(value)
        for name, oracle in (("hs_sha256", lambda v: hashlib.sha256(v).digest()),
                             ("hs_sha256d", double_hash)):
            out = output_buffer()
            checks.same(getattr(library, name)(data, len(value), out), OK, label + " status " + name)
            checks.same(bytes(out), oracle(value), label + " digest " + name)
            checks.case(name)

    for length in fixed_lengths:
        check_hash(bytes((i * 73 + length) % 256 for i in range(length)), f"padding-{length}")
    for index in range(count):
        check_hash(rng.randbytes(rng.randrange(8193)), f"hash-random-{index}")
    for value in (b"", b"abc", GENESIS, rng.randbytes(129)):
        for name, oracle in (("hs_sha256", lambda v: hashlib.sha256(v).digest()),
                             ("hs_sha256d", double_hash)):
            for offset in (0, 16):
                storage = (U8 * (max(len(value), 32) + 32))()
                for index, byte in enumerate(value):
                    storage[index] = byte
                out = ct.cast(ct.byref(storage, offset), U8P)
                checks.same(getattr(library, name)(storage, len(value), out), OK, "alias status " + name)
                checks.same(bytes(storage)[offset:offset + 32], oracle(value), "alias digest " + name)
                checks.case("hashAliases")
    for name in ("hs_sha256", "hs_sha256d"):
        function = getattr(library, name)
        for length in (MAX_INPUT + 1, ct.c_size_t(-1).value):
            out = output_buffer()
            checks.same(function(as_buffer(b"x"), length, out), TOO_LONG, "bounded rejection " + name)
            checks.same(bytes(out), SENTINEL, "length error atomicity " + name)
            checks.case("hashLengthGuards")
        out = output_buffer()
        checks.same(function(None, 1, out), INVALID, "NULL nonempty " + name)
        checks.same(bytes(out), SENTINEL, "NULL error atomicity " + name)
        checks.same(function(None, 0, out), OK, "NULL empty " + name)
        expected = hashlib.sha256(b"").digest() if name == "hs_sha256" else double_hash(b"")
        checks.same(bytes(out), expected, "NULL empty digest " + name)
        checks.same(function(None, 0, None), INVALID, "NULL output " + name)
        checks.case("hashNullGuards")
    for value in (GENESIS, rng.randbytes(80)):
        out = output_buffer()
        checks.same(library.hs_sha256d_header80(as_buffer(value), 80, out), OK, "header80 status")
        checks.same(bytes(out), double_hash(value), "header80 digest")
        checks.case("header80")
    for length in (0, 79, 81, MAX_INPUT + 1):
        out = output_buffer()
        checks.same(library.hs_sha256d_header80(as_buffer(b"x"), length, out), INVALID, "header80 length")
        checks.same(bytes(out), SENTINEL, "header80 error atomicity")
        checks.case("header80Guards")
    checks.same(double_hash(GENESIS)[::-1].hex(),
                "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f",
                "Bitcoin Core genesis display hash")

    coefficients = (0, 1, 0x7f, 0x80, 0xff, 0x100, 0xffff, 0x10000, 0x7fffff)
    compacts = [(exponent << 24) | sign | coefficient
                for exponent in range(256) for sign in (0, 0x800000) for coefficient in coefficients]
    compacts += [rng.getrandbits(32) for _ in range(count)]
    for bits in compacts:
        status, magnitude = decode_compact(bits)
        out = output_buffer()
        checks.same(library.hs_pow_target_from_compact(bits, out), status, f"compact-{bits:08x} status")
        checks.same(bytes(out), magnitude.to_bytes(32, "little") if status == OK else SENTINEL,
                    f"compact-{bits:08x} bytes/atomicity")
        checks.case("compactDecode")

    targets = [0, 1, MAX_U256] + [1 << bit for bit in range(256)]
    targets += [rng.getrandbits(256) for _ in range(count)]
    for index, target in enumerate(targets):
        target_data = as_buffer(target.to_bytes(32, "little"))
        encoded = ct.c_uint32(0xa5a5a5a5)
        checks.same(library.hs_pow_compact_from_target(target_data, ct.byref(encoded)), OK, "encode status")
        checks.same(encoded.value, encode_compact(target), "encode integer oracle")
        status, rounded = decode_compact(encoded.value)
        checks.same(status, ZERO if target == 0 else OK, "encoded range")
        if target:
            checks.same(rounded <= target, True, "compact rounding never increases target")
        checks.case("compactEncode")
        for value in (rng.getrandbits(256), target, max(0, target - 1), min(MAX_U256, target + 1)):
            result = ct.c_int(-17)
            actual = library.hs_pow_hash_meets_target(as_buffer(value.to_bytes(32, "little")),
                                                       target_data, ct.byref(result))
            checks.same(actual, ZERO if target == 0 else OK, f"compare-{index} status")
            checks.same(result.value, -17 if target == 0 else int(value <= target), "compare endian/boundary")
            checks.case("hashTargetCompare")

    for index in range(count):

        bits = (rng.randrange(1, 33) << 24) | rng.randrange(1, 0x800000) if index % 2 else rng.getrandbits(32)
        status, target = decode_compact(bits)
        limit = (0 if index % 19 == 0 else target if status == OK and index % 3 == 0
                 else max(0, target - 1) if status == OK and index % 3 == 1 else rng.getrandbits(256))
        value = rng.getrandbits(256)
        wanted = ZERO if limit == 0 else status
        if wanted == OK and target > limit:
            wanted = ABOVE_LIMIT
        accepted = ct.c_int(-17)
        actual = library.hs_pow_check_compact(as_buffer(value.to_bytes(32, "little")), bits,
                                               as_buffer(limit.to_bytes(32, "little")), ct.byref(accepted))
        checks.same(actual, wanted, "network powLimit status")
        checks.same(accepted.value, int(value <= target) if wanted == OK else -17, "powLimit atomicity/comparison")
        checks.case("compactPowLimit")


def main():
    parser = argparse.ArgumentParser(description=None)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cases", type=int, default=4000)
    parser.add_argument("--seed", type=lambda value: int(value, 0), default=0x4853504f57)
    args = parser.parse_args()
    require(1 <= args.cases <= 20000, "cases must be 1..20000")
    require(0 <= args.seed < (1 << 64), "seed must be an unsigned 64-bit integer")
    require(args.library.is_absolute() and not args.library.is_symlink(), "absolute non-symlink library required")
    require(stat.S_ISREG(args.library.lstat().st_mode) and args.library.stat().st_size <= 16 * 1024 * 1024,
            "library must be a bounded regular file")
    require(args.output.is_absolute() and not args.output.exists() and not args.output.is_symlink(),
            "new absolute receipt path required")
    require(args.output.parent.is_dir() and not args.output.parent.is_symlink(), "regular existing receipt parent required")
    root = Path(__file__).resolve().parent.parent
    sources = ("include/hs_pow.h", "src/pow.c", "tools/differential_pow.py")
    source_hashes = {name: file_hash(root / name) for name in sources}
    library_hash = file_hash(args.library)
    checks = Checks()
    report = {"schemaVersion": 1, "status": "FAILED", "seed": args.seed,
              "requestedRandomCasesPerFamily": args.cases, "sourceSHA256": source_hashes,
              "librarySHA256": library_hash, "pythonVersion": sys.version,
              "oracle": "hashlib SHA256 and unbounded Python integer arithmetic",
              "maxRandomHashBytes": 8192, "sha256ResourceCapBytes": MAX_INPUT,
              "libraryToSourceBindingVerifiedHere": False,
              "poolOrDeviceConnections": False, "vendorNoncePathValidated": False,
              "completeCgminer": False, "deployableFirmware": False}

    with args.output.open("x", encoding="utf-8") as stream:
        os.chmod(args.output, 0o600)
        try:
            library = load_library(args.library)
            run(library, args.cases, args.seed, checks)
            require(file_hash(args.library) == library_hash, "library changed during checks")
            require({name: file_hash(root / name) for name in sources} == source_hashes,
                    "source changed during checks")
            report["status"] = "PASSED"
        except Exception as error:
            report["error"] = type(error).__name__ + ": " + str(error)
        report["casesByFamily"] = checks.groups
        report["totalCases"] = sum(checks.groups.values())
        report["assertions"] = checks.assertions
        json.dump(report, stream, indent=2, sort_keys=True)
        stream.write("\n")
    print(json.dumps({"status": report["status"], "cases": report["totalCases"],
                      "assertions": report["assertions"], "receipt": str(args.output)}))
    return 0 if report["status"] == "PASSED" else 1


if __name__ == "__main__":
    sys.exit(main())
