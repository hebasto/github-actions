#!/usr/bin/env python3
"""
Reproduce "bind: Address already in use" on a port that no process listens on.

Bitcoin Core's `feature_port.py` stops bitcoind and then binds a plain socket
to the node's P2P port. The bind fails when the kernel has handed that port
out as the ephemeral local port of an unrelated outgoing connection.

Part 1 shows the mechanism deterministically. Part 2 shows whether it happens
by chance to a block of ports from the functional test framework's range while
unrelated short-lived connections are being made, as in a parallel test run.
"""

import errno
import platform
import socket
import subprocess
import sys
import threading
import time

# One test's block of P2P ports in Bitcoin Core's functional test framework.
TARGET_PORTS = range(11972, 11984)
CHURN_SECONDS = 30
CHURN_THREADS = 4
CHURN_SERVERS = 32
# Per-thread pause between connections, which caps the rate at about 1000/s.
CHURN_PAUSE = 0.004
SOCKET_TIMEOUT = 5


def ephemeral_range():
    commands = {
        "Linux": ["sysctl", "net.ipv4.ip_local_port_range"],
        "FreeBSD": ["sysctl", "net.inet.ip.portrange.first", "net.inet.ip.portrange.last",
                    "net.inet.ip.portrange.randomized"],
        "OpenBSD": ["sysctl", "net.inet.ip.portfirst", "net.inet.ip.portlast"],
    }
    out = subprocess.run(commands[platform.system()], capture_output=True, text=True).stdout
    return ", ".join(out.split("\n")).strip(", ")


def try_bind(port, reuseaddr):
    """Bind 0.0.0.0:port and return "ok" or the errno name."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        if reuseaddr:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind(("0.0.0.0", port))
            return "ok"
        except OSError as e:
            return errno.errorcode.get(e.errno, str(e.errno))


def listener():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    s.listen(128)
    return s


def part1():
    print("Part 1: bind to the ephemeral port of an unrelated connection")
    with listener() as server:
        client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        client.connect(server.getsockname())
        accepted, _ = server.accept()
        accepted.settimeout(SOCKET_TIMEOUT)
        port = client.getsockname()[1]
        print(f"  connection established:  plain bind: {try_bind(port, False):11}"
              f"  SO_REUSEADDR bind: {try_bind(port, True)}")
        # The client closes first, so its port enters TIME_WAIT.
        client.close()
        accepted.recv(1)
        accepted.close()
        time.sleep(0.5)
        print(f"  connection in TIME_WAIT: plain bind: {try_bind(port, False):11}"
              f"  SO_REUSEADDR bind: {try_bind(port, True)}")


def part2():
    print(f"Part 2: bind to ports {TARGET_PORTS.start}-{TARGET_PORTS.stop - 1} "
          f"during {CHURN_SECONDS} s of unrelated connections")
    servers = [listener() for _ in range(CHURN_SERVERS)]
    stop = threading.Event()
    connections = [0] * CHURN_THREADS
    connect_errors = {}

    def accept_loop(server):
        while True:
            try:
                accepted, _ = server.accept()
            except OSError:
                return
            # Wait for the client to close first.
            accepted.settimeout(SOCKET_TIMEOUT)
            try:
                accepted.recv(1)
            except OSError:
                pass
            accepted.close()

    def churn(index):
        while True:
            for server in servers:
                if stop.is_set():
                    return
                try:
                    with socket.create_connection(server.getsockname(), timeout=SOCKET_TIMEOUT):
                        connections[index] += 1
                except OSError as e:
                    name = errno.errorcode.get(e.errno, type(e).__name__)
                    connect_errors[name] = connect_errors.get(name, 0) + 1
                time.sleep(CHURN_PAUSE)

    for server in servers:
        threading.Thread(target=accept_loop, args=(server,), daemon=True).start()
    threads = [threading.Thread(target=churn, args=(i,)) for i in range(CHURN_THREADS)]
    for t in threads:
        t.start()

    rounds = 0
    plain_failures = {}
    reuseaddr_failures = {}
    first_failure = None
    deadline = time.monotonic() + CHURN_SECONDS
    next_progress = time.monotonic() + 5
    while time.monotonic() < deadline:
        if time.monotonic() >= next_progress:
            next_progress += 5
            print(f"  ... connections: {sum(connections)}, connect errors: {connect_errors or 'none'}, "
                  f"plain bind failures: {sum(plain_failures.values())}")
        rounds += 1
        for port in TARGET_PORTS:
            if try_bind(port, False) != "ok":
                plain_failures[port] = plain_failures.get(port, 0) + 1
                if first_failure is None:
                    first_failure = CHURN_SECONDS - (deadline - time.monotonic())
                if try_bind(port, True) != "ok":
                    reuseaddr_failures[port] = reuseaddr_failures.get(port, 0) + 1
        time.sleep(0.02)

    stop.set()
    for t in threads:
        t.join()
    for server in servers:
        server.close()

    print(f"  connections made: {sum(connections)}, connect errors: {connect_errors or 'none'}")
    print(f"  bind rounds: {rounds}")
    print(f"  plain bind failures by port: {plain_failures or 'none'}")
    print(f"  of those, SO_REUSEADDR bind also failed: {reuseaddr_failures or 'none'}")
    if first_failure is not None:
        print(f"  first failure after {first_failure:.1f} s")
    print(f"  RESULT: {'REPRODUCED' if plain_failures else 'not reproduced'}")


if __name__ == "__main__":
    print(f"{platform.system()} {platform.release()}, Python {platform.python_version()}")
    print(f"Ephemeral ports: {ephemeral_range()}")
    part1()
    part2()
    sys.stdout.flush()
