#!/usr/bin/env python3
"""Loopback-only candidate DB proxy; pause never stops the shared DB cluster."""

import argparse
import asyncio
import json
import os
from pathlib import Path


async def serve(args):
    paused = False
    writers = set()

    async def relay(reader, writer):
        if paused:
            writer.close()
            await writer.wait_closed()
            return
        upstream = None
        try:
            source, upstream = await asyncio.wait_for(
                asyncio.open_connection("127.0.0.1", args.upstream_port), 3
            )
            if paused:
                return
            writers.update((writer, upstream))

            async def copy(src, dst):
                while True:
                    data = await src.read(65536)
                    if not data:
                        return
                    dst.write(data)
                    await dst.drain()

            tasks = [
                asyncio.create_task(copy(reader, upstream)),
                asyncio.create_task(copy(source, writer)),
            ]
            await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
        except (TimeoutError, OSError):
            pass
        finally:
            for item in (writer, upstream):
                if item:
                    writers.discard(item)
                    item.close()
                    try:
                        await item.wait_closed()
                    except OSError:
                        pass

    async def control(reader, writer):
        nonlocal paused
        try:
            action = (await asyncio.wait_for(reader.readline(), 3)).strip()
            if action not in (b"pause", b"resume", b"status"):
                writer.write(b'{"error":"invalid_action"}\n')
            else:
                if action == b"pause":
                    paused = True
                    for conn in tuple(writers):
                        conn.close()
                elif action == b"resume":
                    paused = False
                writer.write(
                    (
                        json.dumps({"paused": paused, "active_streams": len(writers)})
                        + "\n"
                    ).encode()
                )
            await writer.drain()
        finally:
            writer.close()
            await writer.wait_closed()

    path = args.control_socket
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        raise ValueError(
            "control socket already exists; refuse to replace another proxy"
        )
    server = await asyncio.start_server(relay, "127.0.0.1", args.port)
    ctl = await asyncio.start_unix_server(control, str(path))
    os.chmod(path, 0o600)
    print(json.dumps({"status": "ready", "port": args.port}), flush=True)
    async with server, ctl:
        await asyncio.gather(server.serve_forever(), ctl.serve_forever())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=25432)
    parser.add_argument("--upstream-port", type=int, default=5432)
    parser.add_argument("--control-socket", type=Path, required=True)
    asyncio.run(serve(parser.parse_args()))
