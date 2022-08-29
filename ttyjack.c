/* Copyright © 2016-2022 Jakub Wilk <jwilk@jwilk.net>
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/tiocl.h>
#include <linux/vt.h>
#endif

#define PROGRAM_NAME "ttyjack"

static void show_usage(FILE *fp)
{
    fprintf(fp, "Usage: %s [-L] COMMAND [ARG...]\n", PROGRAM_NAME);
    if (fp != stdout)
        return;
    fprintf(fp,
        "\n"
        "Options:\n"
        "  -L          use TIOCLINUX\n"
        "  -h, --help  display this help and exit\n"
    );
}

static void xerror(const char *context)
{
    int orig_errno = errno;
    fprintf(stderr, "%s: ", PROGRAM_NAME);
    errno = orig_errno;
    perror(context);
    exit(EXIT_FAILURE);
}

static int push_fd_c(int fd, char c, size_t *i)
{
    int rc = ioctl(fd, TIOCSTI, &c);
    if (rc < 0) {
        if (*i == 0)
            return -1;
        xerror("TIOCSTI");
    }
    (*i)++;
    return rc;
}

static int push_fd(int fd, char **argv)
{
    size_t i = 0;
    if (fd > 2)
        i++;
    for (; *argv; argv++) {
        for (char *s = *argv; *s; s++) {
            int rc = push_fd_c(fd, *s, &i);
            if (rc < 0)
                return -1;
        }
        char c = argv[1] ? ' ' : '\n';
        int rc = push_fd_c(fd, c, &i);
        if (rc < 0)
            return -1;
    }
    return 0;
}

static void push(char **argv)
{
    int fd;
    int rc;
    for (fd = 0; fd <= 2; fd++) {
        rc = push_fd(fd, argv);
        if (rc == 0)
            return;
    }
    fd = open("/dev/tty", O_RDONLY);
    if (fd < 0)
        xerror("/dev/tty");
    fd = dup2(fd, 3);
    if (fd < 0)
        xerror("dup2()");
    rc = push_fd(fd, argv);
    assert(rc == 0);
}

#if defined(__linux__)

static void xprintf(int fd, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int rc = vdprintf(fd, fmt, ap);
    if (rc < 0)
        xerror("dprintf()");
    va_end(ap);
}

static int paste_fd(int fd, char **argv)
{
    struct {
        char padding;
        char subcode;
        struct tiocl_selection sel;
    } data;
    data.subcode = TIOCL_GETMOUSEREPORTING;
    int rc = ioctl(fd, TIOCLINUX, &data.subcode);
    if (rc < 0) {
        if (fd > 2)
            xerror("TIOCLINUX");
        return -1;
    }
    xprintf(fd, "%s",
        "\033[H"   // move cursor to (1, 1)
        "\033[2J"  // clear screen
    );
    char *tty_name = ttyname(fd);
    if (tty_name == NULL)
        xerror("ttyname()");
    if (strncmp(tty_name, "/dev/tty", 8) == 0) {
        const char *tty_name_n = tty_name + 8;
        if (*tty_name_n == '\0') {
            // Oh well...
            // FIXME: Use /proc/PID/status to figure the actual tty name.
            tty_name = NULL;
        } else {
            char *endptr;
            errno = 0;
            unsigned long n = strtoul(tty_name_n, &endptr, 10);
            if (errno == 0) {
                if (*endptr != '\0')
                    errno = EINVAL;
                if (n > MAX_NR_CONSOLES)
                    errno = ERANGE;
            }
            if (errno == 0) {
                xprintf(fd, "\033[12;%lu]", n);
                tty_name = NULL;
            }
        }
    }
    if (tty_name != NULL) {
        fprintf(stderr, "%s: unexpected tty name: %s\n", PROGRAM_NAME, tty_name);
        exit(1);
    }
    for (; *argv; argv++) {
        xprintf(fd, "%s", *argv);
        xprintf(fd, "%c", argv[1] ? ' ' : '\n');
    }
    data.subcode = TIOCL_SETSEL;
    data.sel.xs = 1;
    data.sel.xe = 1;
    data.sel.ys = 1;
    data.sel.ye = 1;
    data.sel.sel_mode = TIOCL_SELLINE;
    rc = ioctl(0, TIOCLINUX, &data.subcode);
    if (rc < 0) {
        perror("TIOCL_SETSEL");
        return 1;
    }
    data.subcode = TIOCL_PASTESEL;
    rc = ioctl(0, TIOCLINUX, &data.subcode);
    if (rc < 0) {
        perror("TIOCL_PASTESEL");
        return 1;
    }
    return 0;
}

static void paste(char **argv)
{
    int fd;
    int rc;
    for (fd = 0; fd <= 2; fd++) {
        rc = paste_fd(fd, argv);
        if (rc == 0)
            return;
    }
    fd = open("/dev/tty", O_RDWR);
    if (fd < 0)
        xerror("/dev/tty");
    fd = dup2(fd, 3);
    if (fd < 0)
        xerror("dup2()");
    rc = paste_fd(fd, argv);
    assert(rc == 0);
}

#else

static void paste(char **argv)
{
    (void) argv;
    errno = ENOTSUP;
    xerror("-L");
}

#endif

int main(int argc, char **argv)
{
    bool use_paste = false;
    int opt;
    while ((opt = getopt(argc, argv, "Lh-:")) != -1)
        switch (opt) {
        case 'L':
            use_paste = true;
            break;
        case 'h':
            show_usage(stdout);
            exit(EXIT_SUCCESS);
        case '-':
            if (strcmp(optarg, "help") == 0) {
                show_usage(stdout);
                exit(EXIT_SUCCESS);
            }
            /* fall through */
        default:
            show_usage(stderr);
            exit(EXIT_FAILURE);
        }
    if (optind >= argc) {
        show_usage(stderr);
        exit(EXIT_FAILURE);
    }
    argc -= optind;
    argv += optind;
    if (use_paste)
        paste(argv);
    else
        push(argv);
    return 0;
}

/* vim:set ts=4 sts=4 sw=4 et:*/
