#!/usr/bin/env python3
import os
import pty
import select
import signal
import subprocess
import tempfile
import time


OSC_BG_QUERY = b"\x1b]11;?\x1b\\"
CSI_CURSOR_QUERY = b"\x1b[6n"


def read_available(fd, timeout=1.0):
    deadline = time.time() + timeout
    chunks = []
    while time.time() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.05)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 4096)
        except OSError:
            break
        if not chunk:
            break
        chunks.append(chunk)
    return b"".join(chunks)


def main():
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    dtach = os.environ.get("DTACH", os.path.join(repo, "dtach"))
    with tempfile.TemporaryDirectory() as tmp:
        sock = os.path.join(tmp, "hist.sock")
        command = "printf 'before\\n\033]11;?\033\\\\middle\\n\033[6nafter\\n'; sleep 10"
        subprocess.check_call([dtach, "-n", sock, "/bin/sh", "-c", command])
        time.sleep(0.2)

        master, slave = pty.openpty()
        proc = subprocess.Popen([dtach, "-a", sock], stdin=slave, stdout=slave, stderr=slave)
        os.close(slave)
        try:
            output = read_available(master, timeout=1.0)
        finally:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(timeout=1)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=1)
            os.close(master)

        assert b"before" in output, output
        assert b"middle" in output, output
        assert b"after" in output, output
        assert OSC_BG_QUERY not in output, output
        assert CSI_CURSOR_QUERY not in output, output


if __name__ == "__main__":
    main()
