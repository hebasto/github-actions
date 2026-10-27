# connect(2) never returns on loopback, OpenBSD 7.8 to 8.0

`connect-hang.c` starts a listener on 127.0.0.1 that accepts and closes,
and twenty threads that connect to it in a loop. On OpenBSD 7.8, 7.9 and
8.0 one `connect()` sleeps forever within seconds, on wait channel
`netcon`, although `netstat` shows the connection `ESTABLISHED` on both
ends. OpenBSD 7.7 runs clean.

```sh
cc -O2 -Wall -o connect-hang connect-hang.c -lpthread
./connect-hang
```

Exit status 1 with the stuck thread's `netstat` and `ps` lines means the
hang reproduced; 0 means a clean 60-second run.
