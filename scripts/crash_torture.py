import argparse
import random
import selectors
import signal
import socket
import subprocess
import tempfile
import time
from pathlib import Path


def start_server(binary, database, mode, stderr):
    process = subprocess.Popen(
        [str(binary), "--port", "0", "--data-dir", str(database), "--sync-mode", mode],
        stdout=subprocess.PIPE,
        stderr=stderr,
    )
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            if not selector.select(timeout=20):
                raise RuntimeError("server did not start")
            line = process.stdout.readline().split()
        if len(line) != 2 or line[0] != b"LISTENING":
            raise RuntimeError(f"server failed to announce a port: {line}")
        return process, int(line[1])
    except BaseException:
        process.kill()
        process.wait(timeout=10)
        raise


def verify(port, keys):
    with socket.create_connection(("127.0.0.1", port), timeout=10) as sock:
        with sock.makefile("rb") as replies:
            for i in range(0, len(keys), 64):
                batch = keys[i:i + 64]
                sock.sendall(b"".join(b"GET " + key + b"\n" for key in batch))
                for key in batch:
                    response = replies.readline(1024)
                    if response != b"VALUE " + key + b"\n":
                        raise RuntimeError(f"acknowledged key {key!r} returned {response!r}")


def run(args, work):
    acknowledged = []
    current = None
    writer = None
    port = 0
    random_delay = random.Random(args.seed)
    with (work / "server.log").open("w+") as server_log:
        try:
            for cycle in range(1, args.cycles + 2):
                mode = "always" if cycle % 2 else "batch"
                current, port = start_server(args.server, work / "db", mode, server_log)
                if cycle > 1:
                    verify(port, previous)
                if cycle > args.cycles:
                    verify(port, acknowledged)
                    current.terminate()
                    if current.wait(timeout=20) != 0:
                        raise RuntimeError("final server shutdown failed")
                    break

                ack_path = work / f"ack-{cycle}.log"
                writer = subprocess.Popen([str(args.client), str(port), f"cycle-{cycle}", str(ack_path)])
                deadline = time.monotonic() + 10
                while not ack_path.exists() or ack_path.stat().st_size == 0:
                    if writer.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError(f"writer produced no acknowledgement in cycle {cycle}")
                    time.sleep(0.001)
                time.sleep(random_delay.uniform(0.001, 0.010))
                current.kill()
                if current.wait(timeout=10) != -signal.SIGKILL:
                    raise RuntimeError(f"server did not exit from SIGKILL in cycle {cycle}")
                if writer.wait(timeout=10) != 0:
                    raise RuntimeError(f"writer failed in cycle {cycle}")
                previous = ack_path.read_bytes().splitlines()
                if not previous:
                    raise RuntimeError(f"empty acknowledgement log in cycle {cycle}")
                acknowledged.extend(previous)
                if cycle % 50 == 0:
                    print(f"{cycle} kills, {len(acknowledged)} acknowledgements logged", flush=True)
        except BaseException:
            server_log.seek(0)
            print(server_log.read()[-4096:])
            raise
        finally:
            for process in (writer, current):
                if process is not None and process.poll() is None:
                    process.kill()
                    process.wait(timeout=10)
    return len(acknowledged)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", type=Path, default=Path("build-release/strata_server"))
    parser.add_argument("--client", type=Path, default=Path("build-release/torture_client"))
    parser.add_argument("--cycles", type=int, default=500)
    parser.add_argument("--seed", type=int, default=4831)
    parser.add_argument("--output", type=Path, default=Path("results/torture.log"))
    args = parser.parse_args()
    if args.cycles < 1:
        parser.error("cycles must be positive")
    args.server = args.server.resolve()
    args.client = args.client.resolve()
    with tempfile.TemporaryDirectory(prefix="strata-torture-") as temporary:
        count = run(args, Path(temporary))
    summary = (
        f"SIGKILL cycles: {args.cycles}\n"
        f"Random seed: {args.seed}\n"
        f"Sync modes: alternating always and batch\n"
        f"Acknowledged writes verified after each restart and again at the end: {count}\n"
        "Acknowledged writes lost: 0\n"
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(summary)
    print(summary, end="")


if __name__ == "__main__":
    main()
