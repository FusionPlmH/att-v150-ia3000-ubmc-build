/*
 * sockprobe2 -- isolate *why* one wildcard udp socket hears nothing
 *
 * usage: sockprobe2 <port[:flag]...>
 *   flag r = SO_REUSEADDR
 *   flag p = IP_PKTINFO
 *
 * Binds each port as a plain wildcard listener and echoes "pong" to whoever
 * writes.  Running the same binary on several ports with different flags is
 * the cheapest way to tell a VRF/binding problem from a socket-option one.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define MAXL 8

struct listener {
    int fd;
    int port;
    int reuse;
    int pktinfo;
};

static int open_listener(struct listener *l)
{
    struct sockaddr_in addr;
    int one = 1;

    l->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (l->fd < 0)
        return -1;
    if (l->reuse)
        setsockopt(l->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (l->pktinfo)
        setsockopt(l->fd, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(l->port);
    if (bind(l->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "bind %d: %s\n", l->port, strerror(errno));
        close(l->fd);
        l->fd = -1;
        return -1;
    }
    fprintf(stderr, "listening %d reuse=%d pktinfo=%d\n",
            l->port, l->reuse, l->pktinfo);
    return 0;
}

int main(int argc, char **argv)
{
    struct listener ls[MAXL];
    int n = 0, i;

    memset(ls, 0, sizeof(ls));
    for (i = 1; i < argc && n < MAXL; i++) {
        char *spec = argv[i];
        char *colon = strchr(spec, ':');
        struct listener *l = &ls[n];

        l->port = atoi(spec);
        if (colon)
            for (char *f = colon + 1; *f; f++) {
                if (*f == 'r') l->reuse = 1;
                if (*f == 'p') l->pktinfo = 1;
            }
        if (open_listener(l) == 0)
            n++;
    }
    if (!n)
        return 1;

    for (;;) {
        struct pollfd pfd[MAXL];
        char buf[256];
        int k = 0;

        for (i = 0; i < n; i++)
            if (ls[i].fd >= 0) {
                pfd[k].fd = ls[i].fd;
                pfd[k].events = POLLIN;
                pfd[k].revents = 0;
                k++;
            }
        if (poll(pfd, k, 1000) <= 0)
            continue;

        for (i = 0; i < k; i++) {
            struct sockaddr_in from;
            socklen_t flen = sizeof(from);
            ssize_t got;
            int j;

            if (!(pfd[i].revents & POLLIN))
                continue;
            got = recvfrom(pfd[i].fd, buf, sizeof(buf), 0,
                           (struct sockaddr *)&from, &flen);
            if (got <= 0)
                continue;
            for (j = 0; j < n; j++)
                if (ls[j].fd == pfd[i].fd)
                    break;
            fprintf(stderr, "got %zd bytes on %d from %s:%d\n", got,
                    ls[j].port, inet_ntoa(from.sin_addr), ntohs(from.sin_port));
            sendto(pfd[i].fd, "pong", 4, 0, (struct sockaddr *)&from, flen);
        }
    }
}
