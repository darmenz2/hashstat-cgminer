#!/usr/bin/env python3

import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=None)
    parser.add_argument("--supervisor", type=Path, required=True)
    parser.add_argument("--child", type=Path, required=True)
    parser.add_argument("--receipt", type=Path, required=True)
    args = parser.parse_args()
    supervisor, child = str(args.supervisor.resolve()), str(args.child.resolve())
    cases = []
    root = Path(tempfile.mkdtemp(prefix="supervisor-tests-", dir=args.receipt.parent))

    def expect(label, condition, detail=""):
        cases.append({"name": label, "passed": bool(condition), "detail": detail})
        if not condition:
            raise AssertionError(label + ": " + detail)

    def run(label, tail, expected, milliseconds=1500, **kwargs):
        start = time.monotonic()
        result = subprocess.run([supervisor, str(milliseconds), child] + tail,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                timeout=5, **kwargs)
        elapsed = time.monotonic() - start
        expect(label, result.returncode == expected,
               "exit=%s elapsed=%.3fs stderr=%r" % (result.returncode, elapsed, result.stderr))
        return elapsed

    def pids(path):
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            if path.exists():
                parts = path.read_text().split()
                if len(parts) == 2:
                    return [int(value) for value in parts if int(value)]
            time.sleep(0.01)
        raise AssertionError("fixture did not publish PIDs: " + str(path))

    def running(pid):


        result = subprocess.run(["/bin/ps", "-o", "stat=", "-p", str(pid)],
                                capture_output=True, text=True, timeout=3)
        state = result.stdout.strip()
        return bool(state) and not state.startswith("Z")

    def dead(label, owned):
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline and any(running(pid) for pid in owned):
            time.sleep(0.02)
        expect(label, all(not running(pid) for pid in owned), "owned=" + repr(owned))

    def await_fixture_fuse(owned):


        deadline = time.monotonic() + 16
        while time.monotonic() < deadline and any(running(pid) for pid in owned):
            time.sleep(0.05)

    def signal_case(signo):
        path = root / ("signal-%d.pids" % signo)
        process = subprocess.Popen([supervisor, "5000", child, "descendants", str(path)],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        owned = []
        try:
            owned = pids(path)
            process.send_signal(signo)
            _, err = process.communicate(timeout=4)
            expect("forward-signal-%d" % signo, process.returncode == 128 + signo,
                   "exit=%s stderr=%r" % (process.returncode, err))
            dead("signal-group-cleanup-%d" % signo, owned)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=3)
            await_fixture_fuse(owned)

    def parent_death_case(kill_supervisor):
        path = root / ("parent-%s.pids" % kill_supervisor)


        code = ("import subprocess,sys,time; "
                "p=subprocess.Popen(sys.argv[1:]); "
                "print(p.pid,flush=True); time.sleep(30)")
        launcher = subprocess.Popen([sys.executable, "-c", code, supervisor, "5000",
                                     child, "sleep" if kill_supervisor else "descendants", str(path)],
                                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        owned = []
        supervisor_pid = 0
        try:
            supervisor_pid = int(launcher.stdout.readline().strip())
            owned = pids(path)
            if kill_supervisor:
                os.kill(supervisor_pid, signal.SIGKILL)
            else:
                launcher.kill()
                launcher.wait(timeout=3)
            dead("supervisor-death-direct-child" if kill_supervisor else "launcher-death-group", owned)
        finally:
            if launcher.poll() is None:
                launcher.kill()
                launcher.wait(timeout=3)
            await_fixture_fuse(owned)

    success = False
    skipped = []
    try:
        for value in (0, 1, 23, 124, 125, 127, 255):
            run("exit-%d" % value, ["exit", str(value)], value)
        run("signal-exit", ["signal"], 128 + signal.SIGTERM)
        run("stdin-is-devnull", ["stdin"], 0, input=b"must not reach child")
        with (root / "inherited-fd").open("xb") as stream:
            run("inherited-fd-closed", ["fd", str(stream.fileno())], 0, pass_fds=(stream.fileno(),))
        for timeout in ("", "0", "-1", "+1", "1.0", "600001", "9" * 200):
            result = subprocess.run([supervisor, timeout, child, "exit", "0"], capture_output=True, timeout=3)
            expect("reject-timeout-%r" % timeout, result.returncode == 125)
        for command, expected in (("relative", 125), ("/", 125), (str(root / "absent"), 127),
                                  (str(root), 127)):
            result = subprocess.run([supervisor, "500", command], capture_output=True, timeout=3)
            expect("reject-command-" + command, result.returncode == expected)
        noexec = root / "not-executable"
        noexec.write_text("not an executable\n")
        result = subprocess.run([supervisor, "500", str(noexec)], capture_output=True, timeout=3)
        expect("exec-permission-failure", result.returncode == 127)
        run("inherited-ignore-reset", ["signal"], 128 + signal.SIGTERM,
            preexec_fn=lambda: signal.signal(signal.SIGTERM, signal.SIG_IGN))
        run("inherited-sigchld-ignore-reset", ["exit", "23"], 23,
            preexec_fn=lambda: signal.signal(signal.SIGCHLD, signal.SIG_IGN))
        run("inherited-signal-mask-reset", ["signal"], 128 + signal.SIGTERM,
            preexec_fn=lambda: signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGTERM}))
        for mode in ("sleep", "handle-term", "ignore-term", "descendants"):
            path = root / (mode + ".pids")
            owned = []
            try:
                elapsed = run("deadline-" + mode, [mode, str(path)], 124, milliseconds=200)
                owned = pids(path)
                expect("bounded-" + mode, 0.18 <= elapsed < 2, "elapsed=%.3f" % elapsed)
                dead("deadline-cleanup-" + mode, owned)
            finally:
                await_fixture_fuse(owned)
        elapsed = run("blocked-read-deadline", ["blocked-read"], 124, milliseconds=200)
        expect("bounded-blocked-read", elapsed < 2)


        process = subprocess.Popen([supervisor, "200", child, "blocked-write"],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            start = time.monotonic()
            process.wait(timeout=3)
            elapsed = time.monotonic() - start
            process.communicate(timeout=1)
            expect("blocked-write-deadline", process.returncode == 124 and elapsed < 2)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=3)
        path = root / "natural-exit.pids"
        owned = []
        try:
            run("natural-exit-preserved", ["exit-descendant", str(path)], 23)
            owned = pids(path)
            dead("natural-exit-descendant-cleanup", owned)
        finally:
            await_fixture_fuse(owned)
        unrelated_path = root / "unrelated.pids"
        unrelated = subprocess.Popen([child, "ignore-term", str(unrelated_path)])
        try:
            pids(unrelated_path)
            run("isolation-supervised-timeout", ["blocked-read"], 124, milliseconds=100)
            expect("unrelated-process-survives", unrelated.poll() is None)
        finally:
            if unrelated.poll() is None:
                unrelated.kill()
            unrelated.wait(timeout=3)
        for signo in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
            signal_case(signo)
        for iteration in range(12):
            run("startup-race-%02d" % iteration, ["blocked-read"], 124, milliseconds=1)
        if sys.platform.startswith("linux"):
            parent_death_case(False)
            parent_death_case(True)
        else:
            skipped.extend(["Linux launcher-death process-group cleanup", "Linux supervisor-SIGKILL direct-child PDEATHSIG"])
        success = True
    finally:
        receipt = {"schema": "hashstat.test-supervisor-native-tests.v1", "success": success,
                   "platform": sys.platform, "tests": cases, "passed": sum(case["passed"] for case in cases),
                   "skipped": skipped, "hardwareTest": False, "networkAccess": False,
                   "linuxParentDeathExecuted": sys.platform.startswith("linux")}
        with args.receipt.open("x") as stream:
            json.dump(receipt, stream, indent=2)
            stream.write("\n")
        print(json.dumps({"success": success, "passed": receipt["passed"], "skipped": skipped,
                          "receipt": str(args.receipt)}), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
