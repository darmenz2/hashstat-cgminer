# HashStat cgminer

cgminer-based source tree for **HashMiner**, the mining component of HashStat.

#IMPORTANT
At the moment, the full firmware has not been released—only a portion of it; the firmware will be released only after the drivers for the majority of models are working correctly. 
!!!Currently, only the framework is posted in the git repository!!!
#IMPORTANT

The repository contains the cgminer core, portable work and proof-of-work routines, AML/BM1362 protocol components, runtime code, and build tools. The current source version is `0.0.1-dev`. Hardware bring-up is in progress; this snapshot is not a production firmware image.

## Build and test

On a development host with Python 3, GNU Make, and Clang:

```sh
git clone https://github.com/darmenz2/hashstat-cgminer.git
cd hashstat-cgminer
make check
make runtime CC=clang
```

`make check` runs the build-tool and source-export tests. `make runtime` compiles and runs the runtime tests on the host.

For the native cgminer build, cross-compilation prerequisites, and output locations, see [Building](docs/build.md).

## Source layout

| Path | Contents |
| --- | --- |
| `core/` | cgminer source and HashStat integration |
| `src/`, `include/` | Portable components and public headers |
| `runtime/`, `transport/` | Runtime and transport components |
| `tests/` | Test programs and fixtures |
| `tools/` | Build, source-export, and verification scripts |
| `toolchains/` | Pinned toolchain metadata |
| `source-lock.json` | File manifest used to verify the core source inputs |

The upstream cgminer revision is `b8491c66e7e22f23a9edf095dd1337ee581e88bd`. Component provenance and third-party attribution are recorded in [NOTICE](NOTICE).

## Scope

This repository publishes source and development tooling. Cloud services, enrollment services, installers, and complete system images are not included here. A protocol implementation or a successful host build does not by itself establish support for a particular control board or hashboard revision.

## HashStat Cloud and SaaS

HashStat Cloud is the commercial service layer for remote management, diagnostics, updates, and cloud-assisted tuning. Its architecture includes HashCloud and HashCloudControl for mTLS connectivity and authorized remote access, HashStat Agent for device management and updates, HashStat Enrollment for device identity, and HashAiControl for telemetry analysis and tuning. These components are outside this `0.0.1-dev` source snapshot; availability and supported combinations are documented per firmware release.

Cloud access requires an authorized account and device enrollment. Building or modifying HashMiner does not issue cloud credentials or grant access to the hosted service.

### Using the cloud components

Use the supported component set for the installed release. Do not mix cloud binaries from different releases, copy another device's enrollment material, or replace authentication and integrity checks with local substitutes. These changes can interrupt enrollment, updates, diagnostics, or remote access and are outside the supported service configuration.

Where HashDevFee is supplied with a commercial package, it is part of that package's service configuration. Removing or patching it is not a supported way to retain paid cloud features without the agreed service conditions. Applicable fees and behavior during a cloud outage must be disclosed in the release documentation and service terms.

For custom integrations or removal of cloud services, consult the documentation for the installed release or contact HashStat support before changing interdependent components. Keep private keys and device-specific installation or update packages confidential.

These are cloud-service and support conditions, not additional restrictions on the published source. Rights granted by the applicable open-source licenses remain unchanged; see [COMPLIANCE.md](COMPLIANCE.md).

## Contributing

Bug reports should include the source revision, host or board details, the command that failed, and relevant logs. Remove credentials and device enrollment data before posting. See [CONTRIBUTING.md](CONTRIBUTING.md) for test and patch guidelines.

## License

HashStat files marked `GPL-3.0-only` are distributed under GNU GPLv3. Third-party files retain their existing license and copyright notices; see [LICENSE](LICENSE), [NOTICE](NOTICE), and [COMPLIANCE.md](COMPLIANCE.md).

## Donations

BTC: `bc1qdg9m856est5fucvqv80df2zfuy89mfxwa4qxlx`
