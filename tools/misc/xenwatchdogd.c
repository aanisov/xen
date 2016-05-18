#include <err.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <linux/watchdog.h>

#define DEV_NAME "/dev/watchdog"

int fd = -1;

void daemonize(void)
{
    switch (fork()) {
    case -1:
	err(1, "fork");
    case 0:
	break;
    default:
	exit(0);
    }
    umask(0);
    if (setsid() < 0)
	err(1, "setsid");
    if (chdir("/") < 0)
	err(1, "chdir /");
    if (freopen("/dev/null", "r", stdin) == NULL)
        err(1, "reopen stdin");
    if(freopen("/dev/null", "w", stdout) == NULL)
        err(1, "reopen stdout");
    if(freopen("/dev/null", "w", stderr) == NULL)
        err(1, "reopen stderr");
}

int main(int argc, char **argv)
{
    int t, s;
    int ret;

    if (argc < 2)
	errx(1, "usage: %s <timeout> <sleep>", argv[0]);

    daemonize();

    fd = open(DEV_NAME, O_RDWR);
    if (fd < 0)
        err(1, "xenwatchdogd: Failed to open %s\n", DEV_NAME);

    t = strtoul(argv[1], NULL, 0);
    if (t == ULONG_MAX)
	err(1, "strtoul");

    s = t / 2;
    if (argc == 3) {
	s = strtoul(argv[2], NULL, 0);
	if (s == ULONG_MAX)
	    err(1, "strtoul");
    }

    ret = ioctl(fd, WDIOC_SETTIMEOUT, &t);
    if (ret < 0)
	err(1, "xenwatchdogd: Failed to set timeout\n");

    for (;;) {
	ret = ioctl(fd, WDIOC_KEEPALIVE);
	if (ret)
	    err(1, "xenwatchdogd: Failed to kick watchdog\n");

        sleep(s);
    }
}
