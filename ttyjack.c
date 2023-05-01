/* Copyright © 2016-2023 Jakub Wilk <jwilk@jwilk.net>
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
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <linux/major.h>
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
        "  -L          use TIOCLINUX (works only on /dev/ttyN)\n"
        "  -h, --help  show this help message and exit\n"
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

static int ioctl64(int fd, unsigned long request, void *arg)
{
#if __linux__
    // Linux truncates the ioctl request code to 32 bits,
    // but only _after_ the seccomp check:
    // https://bugs.launchpad.net/snapd/+bug/1812973
    // Let's set the high bits in hope to get around seccomp filters
    // that don't take that into account.
    unsigned long request64 = request | (~0UL ^ ~0U);
    return ioctl(fd, request64, arg);
#else
    return ioctl(fd, request, arg);
#endif
}

static int push_fd_c(int fd, char c, size_t *i)
{
    int rc = ioctl64(fd, TIOCSTI, &c);
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

static int get_tty_n(int fd)
{
    struct stat sb;
    int rc = fstat(fd, &sb);
    if (rc < 0)
        xerror("fstat()");
    if ((sb.st_mode & S_IFMT) != S_IFCHR)
        return -1;
    unsigned int sb_major = major(sb.st_rdev);
    unsigned int sb_minor = minor(sb.st_rdev);
    switch (sb_major) {
        case TTYAUX_MAJOR:
            if (sb_minor == 0) {
                unsigned int ctty_dev;
                rc = ioctl(fd, TIOCGDEV, &ctty_dev);
                if (rc < 0)
                    xerror("TIOCGDEV");
                if (major(ctty_dev) == TTY_MAJOR) {
                    unsigned int ctty_minor = minor(ctty_dev);
                    if ((ctty_minor >= MIN_NR_CONSOLES) && (ctty_minor <= MAX_NR_CONSOLES))
                        return ctty_minor;
                }
                return -1;
            }
            break;
        case TTY_MAJOR:
            if ((sb_minor >= MIN_NR_CONSOLES) && (sb_minor <= MAX_NR_CONSOLES))
                return sb_minor;
            break;
    }
    return -1;
}

static int paste_fd(int fd, char **argv)
{
    struct {
        char padding;
        char subcode;
        struct tiocl_selection sel;
    } data;
    data.subcode = TIOCL_GETMOUSEREPORTING;
    int rc = ioctl64(fd, TIOCLINUX, &data.subcode);
    if (rc < 0) {
        if (fd > 2) {
            if (errno == ENOTTY) {
                int tty_n = get_tty_n(fd);
                if (tty_n < 0) {
                    fprintf(stderr, "%s -L must be run on /dev/ttyN\n", PROGRAM_NAME);
                    exit(EXIT_FAILURE);
                }
            }
            xerror("TIOCLINUX");
        }
        return -1;
    }
    xprintf(fd, "%s",
        "\033[H"   // move cursor to (1, 1)
        "\033[2J"  // clear screen
    );
    int tty_n = get_tty_n(fd);
    if (tty_n < 0) {
        char *tty_name = ttyname(fd);
        if (tty_name == NULL)
            xerror("ttyname()");
        fprintf(stderr, "%s: unexpected device: %s\n", PROGRAM_NAME, tty_name);
        exit(EXIT_FAILURE);
    }
    if (tty_n > 0)
        xprintf(fd, "\033[12;%d]", tty_n);
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
    rc = ioctl64(fd, TIOCLINUX, &data.subcode);
    if (rc < 0) {
        perror("TIOCL_SETSEL");
        return 1;
    }
    data.subcode = TIOCL_PASTESEL;
    rc = ioctl64(fd, TIOCLINUX, &data.subcode);
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
    if (rc < 0)
        exit(EXIT_FAILURE);
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
