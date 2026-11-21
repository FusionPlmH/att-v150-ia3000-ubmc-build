/*
 * brktest - check whether a serial BREAK can be asserted on the host UART while
 * is_console_manager keeps it open for reading.
 *
 *   brktest [device] [ms]
 *
 * Opens the tty write-only (no termios changes), raises TIOCSBRK, holds it for
 * the requested time and drops it.  Reading is unaffected, which is the whole
 * point: SOL needs to be able to send BREAK without owning the port.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/ttyS1";
    int ms = argc > 2 ? atoi(argv[2]) : 300;
    int fd;

    fd = open(dev, O_WRONLY | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        printf("open %s: %s\n", dev, strerror(errno));
        return 1;
    }
    printf("opened %s write-only, fd=%d\n", dev, fd);

    if (ioctl(fd, TIOCSBRK, 0) < 0) {
        printf("TIOCSBRK: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    printf("TIOCSBRK ok, holding %d ms\n", ms);
    usleep((useconds_t)ms * 1000);
    if (ioctl(fd, TIOCCBRK, 0) < 0) {
        printf("TIOCCBRK: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    printf("TIOCCBRK ok\n");
    close(fd);
    return 0;
}
