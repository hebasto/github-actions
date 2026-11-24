#!/usr/bin/env python3
"""Connect an HTTP client's socket to itself.

Pick a port nobody listens on, and make http.client connect to it from that
very port. The socket gets its own SYN, which is a TCP simultaneous open with
a single participant, and the client reads back the request it has sent.

Exit status: 0 if the client fails with BadStatusLine on its own request
line, 1 otherwise.
"""

import http.client
import socket
import sys

# Let the kernel pick a free port, and release it.
with socket.socket() as probe:
    probe.bind(("127.0.0.1", 0))
    address = probe.getsockname()

print(f"Connecting from {address[0]}:{address[1]} to {address[0]}:{address[1]}...")
conn = http.client.HTTPConnection(*address, timeout=10, source_address=address)
try:
    conn.connect()
except OSError as e:
    sys.exit(f"NOT connected to itself: {e!r}")
print(f"Connected: local {conn.sock.getsockname()}, peer {conn.sock.getpeername()}.")

conn.request("POST", "/")
try:
    response = conn.getresponse()
except http.client.BadStatusLine as e:
    print(f"http.client.BadStatusLine: {e.line.strip()}")
    if e.line.strip() == "POST / HTTP/1.1":
        print("Connected to itself.")
        sys.exit(0)
    sys.exit("NOT connected to itself.")
sys.exit(f"NOT connected to itself: got a response with status {response.status}.")
