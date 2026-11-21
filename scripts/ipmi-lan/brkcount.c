/*
 * brkcount - does this UART keep a receive BREAK counter?
 *
 * Detecting a BREAK that the *host* sent is the last missing piece of the SOL
 * implementation.  The console fan-out socket is a byte stream, so the line
 * event is gone by the time we see the data; but the tty layer keeps per-port
 * error counters that TIOCGICOUNT exposes, and port->icount.brk is bumped for
 * every framing-error BREAK the UART sees.
 *
 * This probe only reads the counters.  It must never read() the port: the
 * console manager owns that direction and reading here would steal bytes.
 *
 * build (cross):
 *   arm-linux-gnueabihf-gcc --sysroot=$SYS -O2 -o brkcount brkcount.c
 * run:
 *   brkcount /dev/ttyS1 [seconds]
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/serial.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    struct serial_icounter_struct ic;
    const char *dev = argc > 1 ? argv[1] : "/dev/ttyS1";
    int seconds = argc > 2 ? atoi(argv[2]) : 0;
    int fd, prev_brk, i;

    fd = open(dev, O_RDONLY | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        printf("open %s: %s\n", dev, strerror(errno));
        return 1;
    }

    memset(&ic, 0, sizeof(ic));
    if (ioctl(fd, TIOCGICOUNT, &ic) < 0) {
        printf("TIOCGICOUNT %s: %s\n", dev, strerror(errno));
        close(fd);
        return 1;
    }

    printf("TIOCGICOUNT ok on %s\n", dev);
    printf("  brk=%d frame=%d overrun=%d parity=%d rx=%d tx=%d\n",
           ic.brk, ic.frame, ic.overrun, ic.parity, ic.rx, ic.tx);

    prev_brk = ic.brk;
    for (i = 0; i < seconds; i++) {
        sleep(1);
        if (ioctl(fd, TIOCGICOUNT, &ic) < 0)
            break;
        if (ic.brk != prev_brk) {
            printf("  t+%ds BREAK counter advanced %d -> %d\n",
                   i + 1, prev_brk, ic.brk);
            prev_brk = ic.brk;
        }
    }
    if (seconds > 0) {
        if (ioctl(fd, TIOCGICOUNT, &ic) == 0)
            printf("after %ds: brk=%d frame=%d rx=%d (brk advanced %d)\n",
                   seconds, ic.brk, ic.frame, ic.rx, ic.brk - prev_brk);
    }

    close(fd);
    return 0;
}
