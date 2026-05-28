// wd.c — userspace CLI for /dev/procwatch
// Usage: wd <pid>
// Writes the PID to the driver, then streams log lines to stdout.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>

#define DEVICE      "/dev/procwatch"
#define BUF_SIZE    512

static volatile int running = 1;

static void handle_sig(int sig)
{
    (void)sig;
    running = 0;
}

static void print_timestamp(void)
{
    struct timespec ts;
    struct tm tm_info;
    char tbuf[32];

    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm_info);
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tm_info);
    printf("\033[2m%s\033[0m  ", tbuf);   // dim timestamp
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "usage: wd <pid>\n");
        return 1;
    }

    pid_t pid = (pid_t)atoi(argv[1]);
    if (pid <= 0) {
        fprintf(stderr, "wd: invalid pid\n");
        return 1;
    }

    // open device
    int fd = open(DEVICE, O_RDWR);
    if (fd < 0) {
        perror("wd: open " DEVICE);
        fprintf(stderr, "hint: sudo insmod watchdog_drv.ko\n");
        return 1;
    }

    // write PID to driver (as ascii string)
    char pid_str[32];
    int n = snprintf(pid_str, sizeof(pid_str), "%d\n", pid);
    if (write(fd, pid_str, n) < 0) {
        perror("wd: write pid");
        close(fd);
        return 1;
    }

    signal(SIGINT,  handle_sig);
    signal(SIGTERM, handle_sig);

    printf("\033[1mwatchdog\033[0m  pid \033[33m%d\033[0m\n", pid);
    printf("─────────────────────────────────────────────────────\n");
    fflush(stdout);

    // stream loop — blocking read from driver
    char buf[BUF_SIZE];
    while (running) {
        ssize_t r = read(fd, buf, sizeof(buf) - 1);
        if (r < 0) {
            if (errno == EINTR)
                break;
            perror("wd: read");
            break;
        }
        if (r == 0)
            continue;

        buf[r] = '\0';

        // split on newlines for per-line timestamp
        char *line = buf;
        char *nl;
        while ((nl = strchr(line, '\n')) != NULL) {
            *nl = '\0';
            if (strlen(line) > 0) {
                print_timestamp();

                // colour "PROCESS ENDED" red
                if (strstr(line, "PROCESS ENDED")) {
                    printf("\033[31m%s\033[0m\n", line);
                    running = 0;
                } else if (strstr(line, "watching PID")) {
                    printf("\033[32m%s\033[0m\n", line);  // green for start
                } else {
                    printf("%s\n", line);
                }
                fflush(stdout);
            }
            line = nl + 1;
        }

        // if process ended, exit cleanly
        if (!running)
            break;
    }

    printf("─────────────────────────────────────────────────────\n");
    printf("\033[2mwatchdog detached\033[0m\n");

    close(fd);
    return 0;
}
