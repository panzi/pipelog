#define _DEFAULT_SOURCE 1
#define _GNU_SOURCE

#include "pipelog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <time.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>

struct Pipelog_State {
    char *filename; //!< actual formatted filename
    int fd;         //!< opened filename
};

static void handle_sigint(int sig) {
    if (isatty(STDERR_FILENO)) {
        fprintf(stderr, "\n");
    }
}

static volatile bool received_sighup = false;

static void handle_sighup(int sig) {
    received_sighup = true;
}

// UNIX-only
static int make_parent_dirs(const char *path, mode_t mode) {
    char *buf = strdup(path);
    if (buf == NULL) {
        return -1;
    }

    const size_t len = strlen(buf);
    if (len == 0) {
        errno = EINVAL;
        return -1;
    }

    char *endptr = buf + len;
    while (endptr > buf && *endptr == '/') {
        *endptr = 0;
        -- endptr;
    }

    endptr = buf;

    while (*endptr) {
        while (*endptr == '/') {
            ++ endptr;
        }

        while (*endptr != '/' && *endptr) {
            ++ endptr;
        }

        if (*endptr == 0) {
            // last component is the file
            break;
        }

        *endptr = 0;

        if (mkdir(buf, mode) != 0 && errno != EEXIST) {
            free(buf);
            return -1;
        }

        *endptr = '/';
        ++ endptr;
    }

    free(buf);

    return 0;
}

int pipelog(const int fd, const struct Pipelog_Output output[], const size_t count, const unsigned int flags) {
    char buf[BUFSIZ];
    char link_target[PATH_MAX];
    int status = 0;
    size_t init_count = 0;
    struct Pipelog_State *state = calloc(count, sizeof(struct Pipelog_State));
    struct tm local_now;

    if (state == NULL) {
        if (!(flags & PIPELOG_QUIET)) {
            fprintf(stderr, "*** error: allocating memory: %s\n", strerror(errno));
        }
        status = 1;
        goto cleanup;
    }

    {
        const time_t now = time(NULL);
        if (localtime_r(&now, &local_now) == NULL) {
            if (!(flags & PIPELOG_QUIET)) {
                fprintf(stderr, "*** error: getting local time: %s\n", strerror(errno));
            }
            status = 1;
            goto cleanup;
        }

        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGPIPE);
        if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0) {
            if (!(flags & PIPELOG_QUIET)) {
                fprintf(stderr, "*** error: blocking SIGPIPE: %s\n", strerror(errno));
            }
            status = 1;
            goto cleanup;
        }

        if (signal(SIGINT, handle_sigint) == SIG_ERR) {
            if (!(flags & PIPELOG_QUIET)) {
                fprintf(stderr, "*** error: signal(SIGINT, handle_sigint): %s\n", strerror(errno));
            }
            status = 1;
            goto cleanup;
        }

        if (signal(SIGHUP, handle_sighup) == SIG_ERR) {
            if (!(flags & PIPELOG_QUIET)) {
                fprintf(stderr, "*** error: signal(SIGHUP, handle_sighup): %s\n", strerror(errno));
            }
            status = 1;
            goto cleanup;
        }
    }

    bool any_rotate = false;
    for (; init_count < count; ++ init_count) {
        const struct Pipelog_Output *out = &output[init_count];
        struct Pipelog_State *ptr = &state[init_count];

        if (out->filename != NULL) {
            if (out->fd != -1) {
                if (!(flags & PIPELOG_QUIET)) {
                    fprintf(stderr, "*** error: output[%zu]: file descriptor must be -1 when filename given, but was: %d\n", init_count, out->fd);
                }
                errno = EINVAL;
                status = 1;
                goto cleanup;
            }

            bool rotate = strchr(out->filename, '%');

            const char *filename;
            if (rotate) {
                any_rotate = true;

                if (strftime(buf, sizeof(buf), out->filename, &local_now) == 0) {
                    if (!(flags & PIPELOG_QUIET)) {
                        fprintf(stderr, "*** error: output[%zu].filename: cannot format logfile \"%s\": %s\n", init_count, out->filename, strerror(errno));
                    }
                    status = 1;
                    goto cleanup;
                }

                filename = ptr->filename = strdup(buf);
                if (filename == NULL) {
                    if (!(flags & PIPELOG_QUIET)) {
                        fprintf(stderr, "*** error: output[%zu]: cannot allocate string \"%s\": %s\n", init_count, buf, strerror(errno));
                    }
                    status = 1;
                    goto cleanup;
                }
            } else {
                filename = out->filename;
            }

            ptr->fd = open(filename, O_CREAT | O_WRONLY | O_CLOEXEC | O_APPEND, 0644);
            if (ptr->fd < 0 && errno == ENOENT) {
                if (make_parent_dirs(filename, 0755) != 0) {
                    if (!(flags & PIPELOG_QUIET)) {
                        fprintf(stderr, "*** error: output[%zu]: cannot create parent path of \"%s\": %s\n", init_count, filename, strerror(errno));
                    }
                    status = 1;
                    goto cleanup;
                }
                ptr->fd = open(filename, O_CREAT | O_WRONLY | O_CLOEXEC | O_APPEND, 0644);
            }

            if (ptr->fd < 0) {
                if (!(flags & PIPELOG_QUIET)) {
                    fprintf(stderr, "*** error: output[%zu]: cannot open file \"%s\": %s\n", init_count, filename, strerror(errno));
                }
                status = 1;
                goto cleanup;
            }

            if (out->link != NULL) {
                if (make_parent_dirs(out->link, 0755) != 0) {
                    if (!(flags & PIPELOG_QUIET)) {
                        fprintf(stderr, "*** error: output[%zu]: cannot create parent path of \"%s\": %s\n", init_count, out->link, strerror(errno));
                    }
                    status = 1;
                    goto cleanup;
                }

                if (unlink(out->link) != 0 && errno != ENOENT) {
                    if (!(flags & PIPELOG_QUIET)) {
                        fprintf(stderr, "*** error: output[%zu]: cannot unlink \"%s\": %s\n", init_count, out->link, strerror(errno));
                    }
                    status = 1;
                    goto cleanup;
                }

                char *absfilename = realpath(filename, link_target);
                if (absfilename == NULL) {
                    if (!(flags & PIPELOG_QUIET)) {
                        fprintf(stderr, "*** error: output[%zu]: cannot get absolute path of \"%s\": %s\n", init_count, filename, strerror(errno));
                    }
                    status = 1;
                    goto cleanup;
                }

                if (symlink(absfilename, out->link) != 0) {
                    if (!(flags & PIPELOG_QUIET)) {
                        fprintf(stderr, "*** error: output[%zu]: cannot create symbolic link at \"%s\": %s\n", init_count, out->link, strerror(errno));
                    }
                    status = 1;
                    goto cleanup;
                }
            }
        } else {
            if (out->fd < 0) {
                if (!(flags & PIPELOG_QUIET)) {
                    fprintf(stderr, "*** error: output[%zu]: illegal file descriptor: %d\n", init_count, out->fd);
                }
                errno = EINVAL;
                status = 1;
                goto cleanup;
            }

            if (out->link != NULL) {
                if (!(flags & PIPELOG_QUIET)) {
                    fprintf(stderr, "*** error: output[%zu]: link has to be NULL if filename is NULL\n", init_count);
                }
                errno = EINVAL;
                status = 1;
                goto cleanup;
            }

            ptr->fd = out->fd;
        }
    }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGHUP);

    for (;;) {
        bool forced_rotate = false;
        ssize_t rcount = 0;
        if (received_sighup) {
            // pending SIGHUP was delivered when it was unblocked
            forced_rotate = true;
            received_sighup = false;
        } else {
            rcount = read(fd, buf, sizeof(buf));
            if (rcount == 0) {
                break;
            }

            if (rcount < 0) {
                const int errnum = errno;
                if (errnum == EINTR && received_sighup) {
                    forced_rotate = true;
                    received_sighup = false;
                } else {
                    if (!(flags & PIPELOG_QUIET)) {
                        fprintf(stderr, "*** error: reading input: %s\n", strerror(errnum));
                    }
                    status = 1;
                    goto cleanup;
                }
            }
        }

        // defer delivery of SIGHUP until after all log handling
        if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0) {
            if (!(flags & PIPELOG_QUIET)) {
                fprintf(stderr, "*** error: blocking SIGHUP: %s\n", strerror(errno));
            }
            status = 1;
            goto cleanup;
        }

        if (any_rotate) {
            const time_t now = time(NULL);

            if (localtime_r(&now, &local_now) == NULL) {
                if (!(flags & PIPELOG_QUIET)) {
                    fprintf(stderr, "*** error: getting local time: %s\n", strerror(errno));
                }
                status = 1;
                goto cleanup;
            }
        }

        for (size_t index = 0; index < count; ++ index) {
            struct Pipelog_State *ptr = &state[index];
            int outfd = ptr->fd;

            if (ptr->filename) {
                const struct Pipelog_Output *out = &output[index];
                if (strftime(buf, sizeof(buf), out->filename, &local_now) == 0) {
                    if (!(flags & PIPELOG_QUIET)) {
                        fprintf(stderr, "*** error: output[%zu]: cannot format logfile \"%s\": %s\n", index, out->filename, strerror(errno));
                    }
                    status = 1;
                    goto cleanup;
                }

                const bool new_name = strcmp(ptr->filename, buf) != 0;
                if (outfd < 0 || new_name || forced_rotate) {
                    if (outfd >= 0 && close(outfd) != 0) {
                        if (!(flags & PIPELOG_QUIET)) {
                            fprintf(stderr, "*** error: output[%zu]: closing file \"%s\": %s\n", index, ptr->filename, strerror(errno));
                        }
                    }

                    if (new_name) {
                        const size_t len = strlen(buf) + 1;

                        char *filename = realloc(ptr->filename, len);
                        if (filename == NULL) {
                            if (!(flags & PIPELOG_QUIET)) {
                                fprintf(stderr, "*** error: output[%zu]: cannot allocate string \"%s\": %s\n", index, buf, strerror(errno));
                            }
                            status = 1;
                            goto cleanup;
                        }

                        memcpy(filename, buf, len);
                        ptr->filename = filename;
                    }

                    ptr->fd = outfd = open(ptr->filename, O_CREAT | O_WRONLY | O_CLOEXEC | O_APPEND, 0644);
                    if (outfd < 0 && errno == ENOENT) {
                        if (make_parent_dirs(ptr->filename, 0755) != 0) {
                            if (!(flags & PIPELOG_QUIET)) {
                                fprintf(stderr, "*** error: output[%zu]: cannot create parent path of \"%s\": %s\n", index, ptr->filename, strerror(errno));
                            }
                            status = 1;
                            goto cleanup;
                        }
                        ptr->fd = outfd = open(ptr->filename, O_CREAT | O_WRONLY | O_CLOEXEC | O_APPEND, 0644);
                    }

                    if (outfd < 0) {
                        if (!(flags & PIPELOG_QUIET)) {
                            fprintf(stderr, "*** error: output[%zu]: opening file \"%s\": %s\n", index, ptr->filename, strerror(errno));
                        }

                        if ((flags & PIPELOG_EXIT_ON_WRITE_ERROR)) {
                            status = 1;
                            goto cleanup;
                        }
                    } else if (new_name && out->link != NULL) {
                        if (unlink(out->link) != 0 && errno != ENOENT) {
                            if (!(flags & PIPELOG_QUIET)) {
                                fprintf(stderr, "*** error: output[%zu]: cannot unlink \"%s\": %s\n", index, out->link, strerror(errno));
                            }
                            status = 1;
                            goto cleanup;
                        }

                        char *absfilename = realpath(ptr->filename, link_target);
                        if (absfilename == NULL) {
                            if (!(flags & PIPELOG_QUIET)) {
                                fprintf(stderr, "*** error: output[%zu]: cannot get absolute path of \"%s\": %s\n", index, ptr->filename, strerror(errno));
                            }
                            status = 1;
                            goto cleanup;
                        }

                        if (symlink(absfilename, out->link) != 0) {
                            if (!(flags & PIPELOG_QUIET)) {
                                fprintf(stderr, "*** error: output[%zu]: cannot create symbolic link at \"%s\": %s\n", index, out->link, strerror(errno));
                            }
                            status = 1;
                            goto cleanup;
                        }
                    }
                }
            }

            if (outfd > -1) {
                size_t offset = 0;
                while (offset < rcount) {
                    const ssize_t wcount = write(outfd, buf + offset, rcount - offset);
                    if (wcount < 0) {
                        const int errnum = errno;
                        if (!(flags & PIPELOG_QUIET)) {
                            fprintf(stderr, "*** error: output[%zu]: writing output: %s\n", index, strerror(errnum));
                        }

                        if (errnum == EINTR) {
                            status = 1;
                            goto cleanup;
                        }

                        if ((flags & PIPELOG_EXIT_ON_WRITE_ERROR)) {
                            status = 1;
                            goto cleanup;
                        }

                        if (errnum != EAGAIN) {
                            ptr->fd = -1;
                        }
                        break;
                    }
                    offset += wcount;
                }
            }
        }

        if (sigprocmask(SIG_UNBLOCK, &mask, NULL) != 0) {
            if (!(flags & PIPELOG_QUIET)) {
                fprintf(stderr, "*** error: unblocking SIGHUP: %s\n", strerror(errno));
            }
            status = 1;
            goto cleanup;
        }
    }

cleanup:
    for (size_t index = 0; index < init_count; ++ index) {
        struct Pipelog_State *ptr = &state[index];
        if (ptr->fd > -1 && output[index].filename != NULL) {
            // close file descriptors opened by this function, and only those
            close(ptr->fd);
            ptr->fd = -1;
        }
        free(ptr->filename);
        ptr->filename = NULL;
    }
    free(state);
    state = NULL;

    return status;
}
