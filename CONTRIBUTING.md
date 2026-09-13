# Contributing

Keep each change focused on one problem. Describe the behavior being changed, the reason for the change, and how it was tested.

## Bug reports

Include the commit or release, the host operating system and compiler, and the exact command that failed. For device-specific problems, include the miner model, control board, hashboard revision, and firmware version. State the expected result and what happened instead.

Attach the relevant part of the log as text. Remove passwords, pool credentials, private keys, enrollment data, and information identifying other operators. Do not upload installation or update packages containing device-specific secrets.

## Patches

Follow the style of the surrounding code. Keep unrelated formatting changes out of functional patches. Preserve copyright, SPDX, and upstream attribution notices.

Run the checks relevant to the change:

```sh
make check
make runtime CC=clang
```

Changes to the cgminer integration should also be tested with `make native`; see [Building](docs/build.md). State the commands and results in the pull request. Distinguish host tests from hardware tests, and name the board revision when reporting a hardware result.

Core builds validate their inputs against `source-lock.json`. Changes to `core/` or its portable mirror need a corresponding manifest update through the source-export tooling. Do not bypass the input checks to make a build pass.

Use a commit subject that names the affected component and the change, for example `runtime: reject stale job identifiers`. Squash temporary fixups before submitting a patch. Do not include local build output, credentials, or editor state.
