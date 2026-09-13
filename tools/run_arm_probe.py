#!/usr/bin/env python3

import argparse
import hashlib
import io
import json
from pathlib import Path
import stat
import sys

import unicorn
from unicorn import arm_const as arm
from elftools.elf.elffile import ELFFile

PAGE = 4096
STACK = 0x70000000
STACK_SIZE = 0x10000
RETURN = 0x71000000
MAX_INSTRUCTIONS = 1000000


def check(ok, message):
    if not ok:
        raise ValueError(message)


def main():
    parser = argparse.ArgumentParser(description=None)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--linux-selftest", action="store_true")
    parser.add_argument("--write-eintr", type=int, default=0)
    parser.add_argument("--max-write", type=int, default=256)
    parser.add_argument("--write-fault", choices=("none", "zero", "eio", "oversized"), default="none")
    args = parser.parse_args()
    build = args.build
    check(build.is_absolute() and build.is_dir() and not build.is_symlink(), "absolute regular build directory required")
    check(0 <= args.write_eintr <= 16 and (args.linux_selftest or args.write_eintr == 0), "invalid EINTR injection")
    check(1 <= args.max_write <= 256 and (args.linux_selftest or
          (args.max_write == 256 and args.write_fault == "none")), "invalid write injection")
    write_suffix = "" if args.max_write == 256 and args.write_fault == "none" else "-write-%d-%s" % (args.max_write, args.write_fault)
    report_path = build / ("linux-execution-eintr-%d%s.json" % (args.write_eintr, write_suffix) if args.linux_selftest else "arm-execution.json")
    check(not report_path.exists() and not report_path.is_symlink(), "execution receipt already exists")
    receipt = json.loads((build / "build-receipt.json").read_text())
    check(receipt["status"] == "MODULE_BUILD_AND_NATIVE_TESTS_PASSED", "build did not pass")
    path = build / ("hashstat-aml88-selftest.arm" if args.linux_selftest else "hs-reconstruction-probe.arm.elf")
    check(stat.S_ISREG(path.lstat().st_mode) and path.stat().st_size <= 16 * 1024 * 1024, "test ELF must be a bounded regular file")
    raw = path.read_bytes()
    sha = hashlib.sha256(raw).hexdigest()
    check(receipt["outputSHA256"][path.name] == sha, "ELF differs from build receipt")
    source_root = Path(__file__).resolve().parent.parent
    for name, expected in receipt["sourceSHA256"].items():
        relative = Path(name)
        check(not relative.is_absolute() and ".." not in relative.parts, "unsafe receipt source path")
        check(hashlib.sha256((source_root / relative).read_bytes()).hexdigest() == expected, "source changed after build: " + name)
    elf = ELFFile(io.BytesIO(raw))
    check(elf.elfclass == 32 and elf.little_endian and elf["e_machine"] == "EM_ARM" and elf["e_type"] == "ET_EXEC", "wrong ARM ELF format")
    check(elf["e_flags"] & 0xFF000600 == 0x05000400, "expected EABI5 hard-float")
    attributes_section = elf.get_section_by_name(".ARM.attributes")
    check(attributes_section is not None, "missing target attributes")
    attributes = {}
    for subsection in attributes_section.iter_subsections():
        if subsection.header["vendor_name"] != "aeabi":
            continue
        for group in subsection.iter_subsubsections():
            if group.header.tag != "TAG_FILE":
                continue
            for attribute in group.iter_attributes():
                check(attribute.tag not in attributes or attributes[attribute.tag] == attribute.value, "conflicting ARM attributes")
                attributes[attribute.tag] = attribute.value
    for tag, expected in {"TAG_CPU_ARCH": 10, "TAG_CPU_ARCH_PROFILE": 65,
                          "TAG_ARM_ISA_USE": 1, "TAG_FP_ARCH": 4,
                          "TAG_ABI_VFP_ARGS": 1}.items():
        check(attributes.get(tag) == expected, "target attribute mismatch: " + tag)
    symbols = elf.get_section_by_name(".symtab")
    check(symbols is not None, "missing symbol table")
    unresolved = [s.name for s in symbols.iter_symbols() if s.name and s["st_shndx"] == "SHN_UNDEF"]
    check(not unresolved, "unresolved symbols: " + str(unresolved))
    entry_symbols = symbols.get_symbol_by_name("_start" if args.linux_selftest else "hs_arm_probe") or []
    check(len(entry_symbols) == 1 and entry_symbols[0]["st_value"] == elf["e_entry"], "entry is not our test function")
    entry = elf["e_entry"]
    count_symbols = symbols.get_symbol_by_name("hs_arm_probe_case_count") or []
    check(len(count_symbols) == 1 and count_symbols[0]["st_size"] == 4, "missing source-built test count")
    count_address = count_symbols[0]["st_value"]

    uc = unicorn.Uc(unicorn.UC_ARCH_ARM, unicorn.UC_MODE_ARM)
    uc.ctl_set_cpu_model(arm.UC_CPU_ARM_CORTEX_A9)
    pages = {}
    loads = []
    for segment in elf.iter_segments():
        check(segment["p_type"] not in ("PT_DYNAMIC", "PT_INTERP"), "OS/dynamic runtime not allowed")
        if segment["p_type"] != "PT_LOAD":
            continue
        start, size, filesz = segment["p_vaddr"], segment["p_memsz"], segment["p_filesz"]
        check(0 <= filesz <= size <= 16 * 1024 * 1024 and PAGE <= start < start + size < 0x10000000, "invalid ELF load range")
        check(segment["p_offset"] + filesz <= len(raw), "truncated ELF load")
        permissions = ((unicorn.UC_PROT_READ if segment["p_flags"] & 4 else 0) |
                       (unicorn.UC_PROT_WRITE if segment["p_flags"] & 2 else 0) |
                       (unicorn.UC_PROT_EXEC if segment["p_flags"] & 1 else 0))
        for address in range(start & -PAGE, (start + size + PAGE - 1) & -PAGE, PAGE):
            pages[address] = pages.get(address, 0) | permissions
        loads.append(segment)
    check(0 < len(pages) <= 4096 and any(s["p_vaddr"] <= entry < s["p_vaddr"] + s["p_filesz"] and s["p_flags"] & 1 for s in loads), "unmapped test entry")
    for address, permissions in pages.items():
        check(not (permissions & unicorn.UC_PROT_EXEC and permissions & unicorn.UC_PROT_WRITE), "writable executable test page")
        uc.mem_map(address, PAGE, permissions)
    for segment in loads:
        uc.mem_write(segment["p_vaddr"], segment.data())
    check(any(s["p_vaddr"] <= count_address and count_address + 4 <= s["p_vaddr"] + s["p_filesz"] for s in loads), "test count outside file-backed load")
    case_count = int.from_bytes(uc.mem_read(count_address, 4), "little")
    check(1 <= case_count <= 10000, "invalid source-built test count")
    uc.mem_map(STACK, STACK_SIZE, unicorn.UC_PROT_READ | unicorn.UC_PROT_WRITE)
    uc.mem_map(RETURN, PAGE, unicorn.UC_PROT_READ | unicorn.UC_PROT_EXEC)
    uc.reg_write(arm.UC_ARM_REG_CPSR, 0x10)
    uc.reg_write(arm.UC_ARM_REG_C1_C0_2, 0xF << 20)
    uc.reg_write(arm.UC_ARM_REG_FPEXC, 0x40000000)
    uc.reg_write(arm.UC_ARM_REG_FPSCR, 0)
    uc.reg_write(arm.UC_ARM_REG_SP, STACK + STACK_SIZE - 16)
    uc.reg_write(arm.UC_ARM_REG_LR, RETURN)
    state = {"instructions": 0, "interrupts": [], "invalidMemory": []}
    linux = {"exited": False, "exitCode": None, "syscalls": [], "stdout": "", "injectedEINTR": 0,
             "writeFault": args.write_fault, "maxWrite": args.max_write, "unexpected": None}

    def code_hook(_uc, _address, _size, _data):
        state["instructions"] += 1

    def interrupt_hook(emulator, number, _data):
        if args.linux_selftest and number == 2:
            call = emulator.reg_read(arm.UC_ARM_REG_R7)
            linux["syscalls"].append(call)
            if call == 1:
                linux["exited"] = True
                linux["exitCode"] = emulator.reg_read(arm.UC_ARM_REG_R0)
                emulator.emu_stop()
                return
            if call == 4:
                fd = emulator.reg_read(arm.UC_ARM_REG_R0)
                address = emulator.reg_read(arm.UC_ARM_REG_R1)
                length = emulator.reg_read(arm.UC_ARM_REG_R2)
                if fd == 1 and 0 < length <= 256 and len(linux["stdout"]) + length <= 1024:
                    if linux["injectedEINTR"] < args.write_eintr:
                        linux["injectedEINTR"] += 1
                        emulator.reg_write(arm.UC_ARM_REG_R0, 0xfffffffc)
                    elif args.write_fault != "none":
                        emulator.reg_write(arm.UC_ARM_REG_R0,
                            {"zero": 0, "eio": 0xfffffffb, "oversized": length + 1}[args.write_fault])
                    else:
                        written = min(length, args.max_write)
                        linux["stdout"] += bytes(emulator.mem_read(address, written)).decode("ascii", "strict")
                        emulator.reg_write(arm.UC_ARM_REG_R0, written)
                    return
            linux["unexpected"] = {"interrupt": number, "syscall": call}
        state["interrupts"].append(number)
        emulator.emu_stop()

    def invalid_hook(_uc, access, address, size, _value, _data):
        state["invalidMemory"].append({"access": access, "address": hex(address), "size": size})
        return False

    uc.hook_add(unicorn.UC_HOOK_CODE, code_hook)
    uc.hook_add(unicorn.UC_HOOK_INTR, interrupt_hook)
    uc.hook_add(unicorn.UC_HOOK_MEM_INVALID, invalid_hook)
    error = None
    try:
        uc.emu_start(entry, RETURN, timeout=5000000, count=MAX_INSTRUCTIONS)
    except unicorn.UcError as exc:
        error = str(exc)
    returned = uc.reg_read(arm.UC_ARM_REG_PC) == RETURN
    result = uc.reg_read(arm.UC_ARM_REG_R0)
    passed = returned and result == 0 and error is None and not state["interrupts"] and not state["invalidMemory"]
    if args.linux_selftest:
        failed_write = args.write_fault != "none" or args.write_eintr > 8
        linux["expectedExitCode"] = 125 if failed_write else 0
        expected_stdout = "" if failed_write else "HASHSTAT AML88 SOURCE SELFTEST PASS; NO HARDWARE TEST\n"
        passed = (linux["exited"] and linux["exitCode"] == linux["expectedExitCode"] and
                  linux["stdout"] == expected_stdout and
                  linux["injectedEINTR"] == min(args.write_eintr, 9) and linux["unexpected"] is None and
                  error is None and not state["interrupts"] and not state["invalidMemory"])
    report = {"status": "PASSED" if passed else "FAILED", "elfSHA256": sha, "runnerSHA256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), "buildReceiptSHA256": hashlib.sha256((build / "build-receipt.json").read_bytes()).hexdigest(), "unicornVersion": unicorn.__version__, "cpu": "ARM Cortex-A9 with VFP, EABI5 hard-float", "armAttributes": attributes, "entry": hex(entry), "returned": returned, "returnCode": result, "checks": case_count, "error": error, **state, "osOrSyscallsProvided": args.linux_selftest, "linuxSyscallEmulation": linux if args.linux_selftest else None, "hardwareAccess": False, "vendorCodeExecuted": False, "completeCgminer": False}
    with report_path.open("x") as output:
        json.dump(report, output, indent=2)
        output.write("\n")
    print(json.dumps(report))
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
