PYTHON ?= python3
JOBS ?= 2
OUTPUT_PARENT ?= /tmp
CC = clang
NATIVE_CC ?= $(shell command -v clang-21 || command -v clang)
ARM_LD ?= arm-none-eabi-ld

.PHONY: check core native modules runtime
check:
	$(PYTHON) -B -m unittest discover -s tools -p test_linux_core_build.py
	$(PYTHON) -B -m unittest discover -s tools -p test_source_export.py
	$(PYTHON) -B -m unittest discover -s tools -p test_portable_component_builders.py

core:
	$(PYTHON) -B tools/build_cgminer_core.py --source "$(CURDIR)/core" --source-lock "$(CURDIR)/source-lock.json" --output-parent "$(OUTPUT_PARENT)" --jobs $(JOBS) --zig "$(ZIG)" --zig-archive "$(ZIG_ARCHIVE)" --objcopy "$(OBJCOPY)" --objcopy-sha256 "$(OBJCOPY_SHA256)"

native:
	$(PYTHON) -B tools/build_cgminer_core.py --source "$(CURDIR)/core" --source-lock "$(CURDIR)/source-lock.json" --output-parent "$(OUTPUT_PARENT)" --jobs $(JOBS) --native-tests --native-cc "$(NATIVE_CC)"

modules:
	@test -n "$(OUTPUT)" || { echo 'OUTPUT must name a new absolute directory' >&2; exit 2; }
	$(PYTHON) -B tools/build.py --output "$(OUTPUT)" --cc "$(CC)" --arm-ld "$(ARM_LD)"

runtime:
	$(PYTHON) -B tools/test_span.py --cc "$(CC)"
	$(PYTHON) -B tools/test_miner_owner.py --cc "$(CC)"
	$(PYTHON) -B tools/test_hashminer_runtime.py --cc "$(CC)"
