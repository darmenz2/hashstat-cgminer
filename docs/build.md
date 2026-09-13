# Building HashMiner

Run the commands below from the repository root. The top-level Makefile provides separate targets for build-tool tests, runtime tests, native core builds, and ARM builds.

## Host checks

Requirements: Python 3, GNU Make, and Clang. The Python build tools use the standard library.

```sh
make check
make runtime CC=clang
```

The first target runs the Linux core-build, source-export, and portable-component-builder unit tests. The second compiles and runs the miner-owner and HashMiner runtime tests on the host.

## Native core build

The native build also requires Autoconf, Automake, Libtool, and pkg-config. A Debian or Ubuntu development host can install these prerequisites with:

```sh
sudo apt-get update
sudo apt-get install python3 make clang autoconf automake libtool pkg-config
```

Select the compiler by its absolute path:

```sh
make native NATIVE_CC="$(command -v clang)" JOBS=2 OUTPUT_PARENT=/tmp
```

The builder creates a staging directory under `OUTPUT_PARENT`, checks the source manifest, configures cgminer, and builds the native test programs with address and undefined-behavior sanitizers. It runs `hashstat-core-test`, `hashstat-stratum-test`, and `hashstat-supervisor-test`, then checks the miner's version and build-information entry points.

Build logs and a `receipt.json` file are written to the staging directory. The final output prints the receipt path and success status. This is a host-side test build, not a hardware mining test.

## ARM core build

The cross-build requires a Zig executable and its original archive matching the host-specific lock under `toolchains/`:

```sh
make core \
  ZIG=/absolute/path/to/zig \
  ZIG_ARCHIVE=/absolute/path/to/zig-archive.tar.xz \
  JOBS=2 OUTPUT_PARENT=/tmp
```

Use the version, host, and archive recorded in the relevant lock file; do not substitute an unrelated Zig installation. The builder verifies the toolchain and emits an ARM Linux core artifact. It does not produce an installable system image.

The optional `OBJCOPY` and `OBJCOPY_SHA256` variables select a separately verified executable for producing a debug-stripped release artifact. Without them, the builder retains the diagnostic artifact.

## Portable modules

The module build uses Clang and an ARM linker. `OUTPUT` must be a new absolute directory:

```sh
make modules OUTPUT=/tmp/hashminer-modules CC=clang ARM_LD=arm-none-eabi-ld
```

Choose a different output path for each build. For additional options, use `python3 tools/build.py --help` and `python3 tools/build_cgminer_core.py --help`.

## Source verification

`source-lock.json` is part of the build input, not generated clutter. A source mismatch should be investigated rather than worked around. The original source tree is checked again after the core build; a changed source tree causes the build to be rejected.

## Algorithm references

The SHA-256 and compact-target routines use the definitions in
[NIST FIPS 180-4](https://nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.180-4.pdf)
and Bitcoin Core's [compact-target conversion](https://github.com/bitcoin/bitcoin/blob/v29.0/src/arith_uint256.cpp)
and [proof-of-work validation](https://github.com/bitcoin/bitcoin/blob/v29.0/src/pow.cpp).
