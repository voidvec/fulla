#!/usr/bin/env python3
"""Host-side stop/start control for one explicitly leased S07 Redis container.

Run outside the workload container, with the socket under its shared private
fixture directory. No Docker socket is exposed to the candidate or driver.
"""

from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import os
import re
import signal
import socket
import subprocess
from pathlib import Path


class RedisControl:
    def __init__(self, fixture):
        if fixture.get("owner") != "fulla-capacity-s07":
            raise ValueError("not an S07 lease")
        infra = fixture["infrastructure"]
        self.name = infra["redis_container"]
        self.volume = infra["redis_volume"]
        nonce = fixture["owner_nonce"]
        if (
            not re.fullmatch(r"fulla-s07-[a-z0-9-]+", self.name)
            or self.volume != "fulla-s07-redis-" + nonce[:10]
        ):
            raise ValueError("container/volume is outside the explicit S07 lease")
        self.proof = hashlib.sha256(nonce.encode()).hexdigest()
        self.container_id = None
        self.inspect()
        if not self.running:
            raise ValueError("owned Redis must be running before fault-control startup")
        self.paused = False

    def inspect(self):
        result = subprocess.run(
            ["docker", "inspect", self.name],
            check=True,
            capture_output=True,
            text=True,
            timeout=10,
        )
        info = json.loads(result.stdout)[0]
        if self.container_id and info["Id"] != self.container_id:
            raise ValueError("owned Redis container was replaced")
        mounts = [m for m in info["Mounts"] if m["Destination"] == "/data"]
        if (
            len(mounts) != 1
            or mounts[0].get("Name") != self.volume
            or info["Name"] != "/" + self.name
        ):
            raise ValueError("owned Redis mount/identity mismatch")
        self.container_id = info["Id"]
        self.running = bool(info["State"]["Running"])

    def action(self, action, proof):
        if not hmac.compare_digest(proof, self.proof) or action not in (
            "pause",
            "resume",
            "status",
        ):
            raise ValueError("invalid lease proof/action")
        self.inspect()
        if action != "status":
            # Restore even if Docker stops the process but its response is lost.
            if action == "pause":
                self.paused = True
            subprocess.run(
                ["docker", "stop" if action == "pause" else "start", self.container_id],
                check=True,
                capture_output=True,
                timeout=20,
            )
            self.inspect()
            self.paused = not self.running
            if self.paused != (action == "pause"):
                raise RuntimeError("owned Redis state does not match control action")
        else:
            self.paused = not self.running
        return {"paused": self.paused}

    def restore(self):
        if self.paused:
            self.action("resume", self.proof)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--socket", type=Path, required=True)
    args = parser.parse_args()
    control = RedisControl(json.loads(args.fixture.read_text()))

    def stop_service(_signum, _frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, stop_service)
    # Refuse to unlink a previous service's socket or any other file.
    if args.socket.exists():
        raise ValueError("control socket already exists")
    with socket.socket(socket.AF_UNIX) as listener:
        listener.bind(str(args.socket))
        os.chmod(args.socket, 0o600)
        listener.listen(1)
        print('{"status":"ready"}', flush=True)
        try:
            while True:
                connection, _ = listener.accept()
                with connection:
                    connection.settimeout(3)
                    request = connection.recv(1024)
                    try:
                        value = json.loads(request)
                        result = control.action(value["action"], value["proof"])
                    except (
                        ValueError,
                        KeyError,
                        OSError,
                        RuntimeError,
                        subprocess.SubprocessError,
                    ):
                        result = {"error": "control_failed"}
                    connection.sendall(json.dumps(result).encode())
        finally:
            control.restore()
            args.socket.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
