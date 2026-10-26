# Blocking `connect(2)` on loopback never returns on OpenBSD

Reproducer for the stalled connections behind
[bitcoin/bitcoin#36099](https://github.com/bitcoin/bitcoin/issues/36099):
a blocking `connect(2)` to a loopback TCP listener does not return, although
the kernel reports the connection `ESTABLISHED` on both ends and the
listener side has already accepted it.

`repro.c` mirrors the shape of the failing functional test. A client process
opens many connections at once to a proxy; each proxy connection is served by
a handler thread that creates a fresh listener on `127.0.0.1:0`, hands it to
a `kqueue` acceptor thread, then does a blocking `connect()` to it and relays
bytes both ways. A watchdog reports any `connect()` pending for longer than
`-t` seconds together with `netstat`, `fstat` and the threads' wait channels.

```sh
cc -O2 -Wall -Wextra -o repro repro.c -lpthread
./repro -n 20 -r 300 -t 10     # 20 connections per round, 300 rounds
./repro -b 5                   # proxy backlog 5, as in the Python SOCKS5 server
./repro -L                     # same, without IP_PORTRANGE_HIGH on the listeners
```

Exit status 0 means every round completed, 2 means a hang was detected.
