//usr/bin/env cc -std=c17 -Wall -Wextra -g -o /tmp/hello-server "$0" && exec /tmp/hello-server "$@"
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>
#include <signal.h>
#include <err.h>

int main() {
	signal(SIGPIPE, SIG_IGN);
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		err(1, "socked");

	int optval = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0)
		err(1, "setsockopt(SO_REUSEADDR)");

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(12345),
		.sin_addr = { .s_addr = htonl(INADDR_LOOPBACK) },
	};
	if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)))
		err(1, "bind");
	if (listen(fd, 1))
		err(1, "listen");
	while (1) {
		printf("listening on tcp:127.0.0.1:12345\n");
		int client_fd = accept(fd, NULL, NULL);
		if (client_fd < 0)
			continue;
		printf("client connected\n");
		struct timespec ts = {0, 400000000};
		for (int i = 3; i--;) {
			if (write(client_fd, "hello\n", 6) <= 0) {
				printf("client disconnected\n");
				break;
			}
			nanosleep(&ts, NULL);
		}
		close(client_fd);
	}
	return 0;
}
