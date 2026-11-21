/*
 * conprobe - inspect the uBMC console fan-out socket
 *
 *   conprobe [-w TEXT] [-t SECONDS] [PATH]
 *
 * Connects to /var/run/console_manager_socket (served by is_console_manager),
 * optionally writes TEXT, then hexdumps everything that arrives for SECONDS.
 * Used to work out whether the socket is a broadcast fan-out and whether it
 * accepts input, before wiring SOL to it.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *path = "/var/run/console_manager_socket";
    const char *write_arg = NULL;
    int seconds = 4, fd, i, n;
    struct sockaddr_un sa;
    struct timeval tv;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0 && i + 1 < argc)
            write_arg = argv[++i];
        else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
            seconds = atoi(argv[++i]);
        else
            path = argv[i];
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        printf("socket: %s\n", strerror(errno));
        return 1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        printf("connect %s: %s\n", path, strerror(errno));
        return 1;
    }
    printf("connected to %s\n", path);

    if (write_arg) {
        ssize_t w = write(fd, write_arg, strlen(write_arg));
        printf("wrote %d of %d bytes: ", (int)w, (int)strlen(write_arg));
        for (i = 0; i < (int)strlen(write_arg); i++)
            printf("%02x ", (unsigned char)write_arg[i]);
        printf("\n");
    }

    printf("reading for %d s:\n", seconds);
    fflush(stdout);
    for (;;) {
        fd_set rfds;
        unsigned char buf[512];
        int r;

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = seconds;
        tv.tv_usec = 0;
        r = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            printf("select: %s\n", strerror(errno));
            break;
        }
        if (r == 0) {
            printf("  (timeout, nothing more)\n");
            break;
        }
        n = (int)read(fd, buf, sizeof(buf));
        if (n <= 0) {
            printf("  read %d (eof/err %s)\n", n, n < 0 ? strerror(errno) : "-");
            break;
        }
        printf("  %d bytes:", n);
        for (i = 0; i < n && i < 64; i++)
            printf(" %02x", buf[i]);
        printf("  |");
        for (i = 0; i < n && i < 64; i++)
            putchar((buf[i] >= 32 && buf[i] < 127) ? buf[i] : '.');
        printf("|\n");
        fflush(stdout);
        seconds = 1;                    /* keep reading while data flows */
    }
    close(fd);
    return 0;
}
