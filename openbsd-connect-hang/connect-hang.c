/*
 * connect(2) on a loopback TCP socket can sleep forever on OpenBSD 7.8, 7.9
 * and 8.0, although the connection is ESTABLISHED on both ends.
 *
 * Twenty threads repeatedly connect to a listener on 127.0.0.1 whose only
 * job is to accept and close. Within seconds one connect() never returns:
 * the thread sleeps on wait channel "netcon" (sys_connect waiting for
 * SS_ISCONNECTING to clear), netstat shows the connection ESTABLISHED, and
 * the acceptor has already accepted it. OpenBSD 7.7 runs clean.
 *
 * Build and run:
 *     cc -O2 -Wall -o connect-hang connect-hang.c -lpthread
 *     ./connect-hang
 *
 * Exits 1 with diagnostics when a connect() is stuck, 0 after a clean run.
 */

#include <sys/socket.h>
#include <netinet/in.h>
#include <err.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define THREADS 20
#define STUCK_SECONDS 10
#define RUN_SECONDS 60

static struct sockaddr_in addr;
static volatile time_t connecting[THREADS];

static void *
acceptor(void *arg)
{
	int lfd = *(int *)arg;

	for (;;) {
		int c = accept(lfd, NULL, NULL);
		if (c >= 0)
			close(c);
	}
	return NULL;
}

static void *
connector(void *arg)
{
	int i = *(int *)arg;
	char c;

	for (;;) {
		int s = socket(AF_INET, SOCK_STREAM, 0);
		if (s < 0)
			err(1, "socket");
		connecting[i] = time(NULL);
		if (connect(s, (struct sockaddr *)&addr, sizeof addr) < 0)
			err(1, "connect");
		connecting[i] = 0;
		/* Let the acceptor close first, so TIME_WAIT stays on its side. */
		(void)read(s, &c, 1);
		close(s);
	}
	return NULL;
}

int
main(void)
{
	int lfd, ids[THREADS], i;
	socklen_t len = sizeof addr;
	pthread_t t;
	time_t start = time(NULL);
	char cmd[128];

	lfd = socket(AF_INET, SOCK_STREAM, 0);
	if (lfd < 0)
		err(1, "socket");
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0)
		err(1, "bind");
	if (listen(lfd, 128) < 0)
		err(1, "listen");
	if (getsockname(lfd, (struct sockaddr *)&addr, &len) < 0)
		err(1, "getsockname");
	printf("listening on 127.0.0.1:%u, %d threads connecting\n",
	    ntohs(addr.sin_port), THREADS);

	if (pthread_create(&t, NULL, acceptor, &lfd) != 0)
		err(1, "pthread_create");
	for (i = 0; i < THREADS; i++) {
		ids[i] = i;
		if (pthread_create(&t, NULL, connector, &ids[i]) != 0)
			err(1, "pthread_create");
	}

	while (time(NULL) - start < RUN_SECONDS) {
		sleep(1);
		for (i = 0; i < THREADS; i++) {
			time_t since = connecting[i];
			if (since != 0 && time(NULL) - since >= STUCK_SECONDS) {
				printf("connect() in thread %d stuck for %lld s\n",
				    i, (long long)(time(NULL) - since));
				fflush(stdout);
				snprintf(cmd, sizeof cmd,
				    "netstat -an -p tcp | grep '\\.%u ' | grep -v TIME_WAIT",
				    ntohs(addr.sin_port));
				(void)system(cmd);
				snprintf(cmd, sizeof cmd,
				    "ps -H -o tid,wchan,state -p %d", getpid());
				(void)system(cmd);
				return 1;
			}
		}
	}
	printf("no hang after %d s\n", RUN_SECONDS);
	return 0;
}
