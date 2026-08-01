#!/usr/bin/env python3
"""Suite 9 — the terminal viewer, end to end, under a real pty.

Why Python and not vitest like the rest: the viewer refuses to start unless
stdin *and* stdout are terminals, so a test has to allocate a pty. The existing
terminal_http/terminal_cdp suites do that through `script -q -c`, which is GNU
syntax — macOS ships BSD `script`, which has no -c, so both skip themselves
there and the viewer goes untested on a developer's own machine. pty.fork() is
in the standard library and behaves the same on both.

Run: python3 tests/e2e/terminal_e2e.py [--bin PATH]
Exits non-zero on the first failure, and prints one line per case.
"""
import argparse, os, pty, re, select, signal, struct, subprocess, sys, termios, fcntl, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CANDIDATES = [
    os.path.join(ROOT, "build/anoa.app/Contents/MacOS/anoa"),  # macOS bundle
    os.path.join(ROOT, "build/anoa"),                          # Linux / Windows
]
PORT = int(os.environ.get("ANOA_E2E_PORT", "9466"))

passed = failed = 0


def check(name, ok, detail=""):
    global passed, failed
    if ok:
        passed += 1
        print(f"  PASS  {name}")
    else:
        failed += 1
        print(f"  FAIL  {name}{(': ' + detail) if detail else ''}")


class Viewer:
    """The viewer under a pty, drained continuously.

    Draining matters: the viewer writes a screenful per frame, and a test that
    stops reading blocks it in write() — where it can no longer poll the signal
    flags, so it looks wedged when it is only unread.
    """

    def __init__(self, binary, args, rows=30, cols=110):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.execvp(binary, [binary] + args)
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        self.buf = b""

    def drain(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.1)
            if r:
                try:
                    self.buf += os.read(self.fd, 200000)
                except OSError:
                    return

    def clear(self):
        """Forget everything read so far.

        Needed before testing that something *stops* being drawn: status_row()
        reports the last reverse-video row in the buffer, and a hidden bar draws
        no new one — so without this the stale row from before the toggle is
        what gets read back.
        """
        self.buf = b""

    def send(self, data, settle=1.0):
        os.write(self.fd, data)
        self.drain(settle)

    def status_row(self):
        rows = re.findall(rb"\x1b\[7m(.*?)\x1b\[0m", self.buf, re.S)
        if not rows:
            return ""
        text = re.sub(rb"\x1b\[[0-9;?]*[ -/]*[@-~]", b"", rows[-1]).decode("utf-8", "replace")
        return "".join(c for c in text if c.isprintable()).strip()

    def alive(self):
        return os.waitpid(self.pid, os.WNOHANG)[0] == 0

    def quit(self, grace=6.0):
        """Ctrl-C, then wait. Returns seconds-to-exit, or None if it never did."""
        os.write(self.fd, b"\x03")
        sent = time.time()
        while time.time() - sent < grace:
            if not self.alive():
                return time.time() - sent
            self.drain(0.1)
        return None

    def kill(self):
        try:
            os.kill(self.pid, signal.SIGKILL)
            os.waitpid(self.pid, 0)
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=next((c for c in CANDIDATES if os.path.exists(c)), None))
    args = ap.parse_args()
    if not args.bin or not os.path.exists(args.bin):
        print("no built binary — looked in:\n  " + "\n  ".join(CANDIDATES))
        return 1
    binary = args.bin

    print("Terminal E2E (Suite 9)")

    # TERM-E2E-01: with nothing listening, the viewer hosts its own browser.
    v = Viewer(binary, ["terminal", "--gfx", "halfblock"])
    v.drain(7)
    row = v.status_row()
    check("hosts its own browser when nothing is listening",
          "embedded" in row, row[:70])
    check("renders frames", len(v.buf) > 100_000, f"{len(v.buf)} bytes")
    v.kill()

    # Start a browser for the rest.
    browser = subprocess.Popen(
        [binary, "--headless", "--no-sandbox", "--port", str(PORT)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + 30
    while time.time() < deadline:
        if subprocess.run([binary, "status", "--port", str(PORT)],
                          capture_output=True).returncode == 0:
            break
        time.sleep(0.5)

    try:
        # TERM-E2E-02: pointed at a running browser it attaches over /render/*
        # rather than starting a second one.
        v = Viewer(binary, ["terminal", "--term-port", str(PORT), "--gfx", "halfblock"])
        v.drain(7)
        row = v.status_row()
        check("attaches to a running browser", f"http 127.0.0.1:{PORT}" in row, row[:70])

        # TERM-E2E-03: Ctrl-L opens the prompt, and it takes the keyboard.
        v.send(b"\x0c", 1.0)
        check("ctrl-l opens the address prompt", "URL:" in v.status_row(), v.status_row()[:70])
        v.send(b"example.com", 1.0)
        check("typing goes to the prompt, not the page",
              "example.com" in v.status_row(), v.status_row()[:70])

        # TERM-E2E-04: Enter navigates, and the page really moves — checked
        # from outside, through the browser, not from the status bar.
        v.send(b"\r", 4.0)
        url = subprocess.run([binary, "eval", "location.href", "--port", str(PORT)],
                             capture_output=True, text=True).stdout.strip()
        check("enter navigates the real page", "example.com" in url, url)

        # TERM-E2E-05: ctrl-c inside the prompt cancels, and does not quit.
        v.send(b"\x0c", 0.6)
        v.send(b"\x03", 0.6)
        check("ctrl-c in the prompt cancels without quitting", v.alive())

        # TERM-E2E-06: ctrl-b stops the status row being drawn at all.
        #
        # Clear *after* the keystroke has settled, not before: the frame timer
        # runs at 30fps, so between clearing and the toggle taking effect a tick
        # would have drawn the old bar and the check would read that.
        v.send(b"\x02", 1.5)
        v.clear()
        v.drain(1.5)
        check("ctrl-b hides the status bar", b"\x1b[7m" not in v.buf, v.status_row()[:70])

        v.send(b"\x02", 1.5)
        check("ctrl-b brings it back", "anoa terminal" in v.status_row(), v.status_row()[:70])

        # TERM-E2E-07: it leaves when asked, and hands the terminal back.
        # Asserted on the exit sequence alone — the buffer was cleared above, so
        # the matching ?1049h is no longer in it to anchor against.
        took = v.quit()
        check("ctrl-c quits", took is not None, "never exited")
        check("restores the alt screen on exit",
              "?1049l" in v.buf.decode("utf-8", "replace"))
        if took is None:
            v.kill()
    finally:
        browser.kill()
        browser.wait()

    print(f"\nTerminal E2E: {passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
