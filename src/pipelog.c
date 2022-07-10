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
#include <poll.h>

#define SPLICE_SIZE ((size_t)2 * 1024 * 1024 * 1024)

struct Pipelog_State {
    char *filename; //!< actual formatted filename
    int fd;         //!< opened filename
};

static volatile bool received_sighup = false;

static void handle_sighup(int sig) {
    received_sighup = true;
}

// logic works for UNIX-only
int make_parent_dirs(const char *path, mode_t mode) {
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

static int get_outfd(const struct Pipelog_Output output[], struct Pipelog_State state[], size_t index, const struct tm *local_now, unsigned int flags) {
    char buf[BUFSIZ > PATH_MAX ? BUFSIZ : PATH_MAX];
    struct Pipelog_State *ptr = &state[index];
    int outfd = ptr->fd;
    sigset_t mask;
    bool unblock_sighup = false;

    if (ptr->filename) {
        const struct Pipelog_Output *out = &output[index];
        if (strftime(buf, sizeof(buf), out->filename, local_now) == 0) {
            if (!(flags & PIPELOG_QUIET)) {
                const int errnum = errno;
                fprintf(stderr, "*** error: output[%zu]: cannot format logfile \"%s\": %s\n", index, out->filename, strerror(errnum));
                errno = errnum;
            }
            outfd = -1;
            goto cleanup;
        }

        const bool new_name = strcmp(ptr->filename, buf) != 0;
        if (outfd < 0 || new_name || (flags & PIPELOG_FORCE_ROTATE)) {
            // defer delivery of SIGHUP until after all log handling
            if (flags & PIPELOG_BLOCK_SIGHUP) {
                sigemptyset(&mask);
                sigaddset(&mask, SIGHUP);

                if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0) {
                    if (!(flags & PIPELOG_QUIET)) {
                        const int errnum = errno;
                        fprintf(stderr, "*** error: blocking SIGHUP: %s\n", strerror(errnum));
                        errno = errnum;
                    }
                    outfd = -1;
                    goto cleanup;
                }

                unblock_sighup = true;
            }

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
                        const int errnum = errno;
                        fprintf(stderr, "*** error: output[%zu]: cannot allocate string \"%s\": %s\n", index, buf, strerror(errnum));
                        errno = errnum;
                    }
                    outfd = -1;
                    goto cleanup;
                }

                memcpy(filename, buf, len);
                ptr->filename = filename;
            }

            const int open_flags = flags & PIPELOG_SPLICE ?
                O_CREAT | O_RDWR | O_CLOEXEC :
                O_CREAT | O_WRONLY | O_CLOEXEC | O_APPEND;
            ptr->fd = outfd = open(ptr->filename, open_flags, 0644);
            if (outfd < 0 && errno == ENOENT) {
                if (make_parent_dirs(ptr->filename, 0755) != 0) {
                    if (!(flags & PIPELOG_QUIET)) {
                        const int errnum = errno;
                        fprintf(stderr, "*** error: output[%zu]: cannot create parent path of \"%s\": %s\n", index, ptr->filename, strerror(errnum));
                        errno = errnum;
                    }
                    outfd = -1;
                    goto cleanup;
                }
                ptr->fd = outfd = open(ptr->filename, open_flags, 0644);
            }

            if (outfd < 0) {
                if (!(flags & PIPELOG_QUIET)) {
                    const int errnum = errno;
                    fprintf(stderr, "*** error: output[%zu]: opening file \"%s\": %s\n", index, ptr->filename, strerror(errnum));
                    errno = errnum;
                }

                outfd = -1;
                goto cleanup;
            } else {
                if ((flags & PIPELOG_SPLICE) && lseek(outfd, 0, SEEK_END) == (off_t)-1) {
                    const int errnum = errno;
                    if (errnum != EPIPE) {
                        if (!(flags & PIPELOG_QUIET)) {
                            fprintf(stderr, "*** error: output[%zu]: seeking file to end \"%s\": %s\n", index, ptr->filename, strerror(errnum));
                        }
                        if (flags & PIPELOG_EXIT_ON_WRITE_ERROR) {
                            close(outfd);
                            ptr->fd = outfd = -1;
                            errno = errnum;
                        }
                        goto cleanup;
                    }
                }

                if (new_name && out->link != NULL) {
                    if (unlink(out->link) != 0 && errno != ENOENT) {
                        const int errnum = errno;
                        if (!(flags & PIPELOG_QUIET)) {
                            fprintf(stderr, "*** error: output[%zu]: cannot unlink \"%s\": %s\n", index, out->link, strerror(errnum));
                        }
                        if (flags & PIPELOG_EXIT_ON_WRITE_ERROR) {
                            close(outfd);
                            ptr->fd = outfd = -1;
                            errno = errnum;
                        }
                        goto cleanup;
                    }

                    char *absfilename = realpath(ptr->filename, buf);
                    if (absfilename == NULL) {
                        const int errnum = errno;
                        if (!(flags & PIPELOG_QUIET)) {
                            fprintf(stderr, "*** error: output[%zu]: cannot get absolute path of \"%s\": %s\n", index, ptr->filename, strerror(errnum));
                        }
                        if (flags & PIPELOG_EXIT_ON_WRITE_ERROR) {
                            close(outfd);
                            ptr->fd = outfd = -1;
                            errno = errnum;
                        }
                        goto cleanup;
                    }

                    if (symlink(absfilename, out->link) != 0) {
                        const int errnum = errno;
                        if (!(flags & PIPELOG_QUIET)) {
                            fprintf(stderr, "*** error: output[%zu]: cannot create symbolic link at \"%s\": %s\n", index, out->link, strerror(errnum));
                        }
                        if (flags & PIPELOG_EXIT_ON_WRITE_ERROR) {
                            close(outfd);
                            ptr->fd = outfd = -1;
                            errno = errnum;
                        }
                        goto cleanup;
                    }
                }
            }
        }
    }

cleanup:

    if (unblock_sighup) {
        const int errnum = errno;
        if (sigprocmask(SIG_UNBLOCK, &mask, NULL) != 0) {
            if (!(flags & PIPELOG_QUIET)) {
                fprintf(stderr, "*** error: unblocking SIGHUP: %s\n", strerror(errno));
            }
        }
        errno = errnum;
    }

    return outfd;
}

int pipelog(const int fd, const struct Pipelog_Output output[], const size_t count, const unsigned int flags) {
    char buf[BUFSIZ];
    char link_target[PATH_MAX];
    int status = PIPELOG_SUCCESS;
    size_t init_count = 0;
    struct Pipelog_State *state = calloc(count, sizeof(struct Pipelog_State));
    struct tm local_now;
    sighandler_t old_handle_sighup = SIG_ERR;

    if (state == NULL) {
        if (!(flags & PIPELOG_QUIET)) {
            fprintf(stderr, "*** error: allocating memory: %s\n", strerror(errno));
        }
        status = PIPELOG_ERROR;
        goto cleanup;
    }

    {
        const time_t now = time(NULL);
        if (localtime_r(&now, &local_now) == NULL) {
            if (!(flags & PIPELOG_QUIET)) {
                fprintf(stderr, "*** error: getting local time: %s\n", strerror(errno));
            }
            status = PIPELOG_ERROR;
            goto cleanup;
        }

        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGPIPE);
        sigaddset(&mask, SIGHUP);
        if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0) {
            if (!(flags & PIPELOG_QUIET)) {
                fprintf(stderr, "*** error: blocking SIGPIPE and SIGHUP: %s\n", strerror(errno));
            }
            status = PIPELOG_ERROR;
            goto cleanup;
        }

        if ((old_handle_sighup = signal(SIGHUP, handle_sighup)) == SIG_ERR) {
            if (!(flags & PIPELOG_QUIET)) {
                fprintf(stderr, "*** error: signal(SIGHUP, handle_sighup): %s\n", strerror(errno));
            }
            status = PIPELOG_ERROR;
            goto cleanup;
        }
    }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGHUP);

    bool use_splice = count == 1 && !(flags & PIPELOG_NO_SPLICE);

    if (use_splice) {
        const int infd_flags = fcntl(fd, F_GETFL, 0);
        if (infd_flags == -1) {
            if (!(flags & PIPELOG_QUIET)) {
                fprintf(stderr, "*** error: getting flags of input file descriptor: %s\n", strerror(errno));
            }
            use_splice = false;
        } else if (!(infd_flags & O_NONBLOCK) && fcntl(fd, F_SETFL, infd_flags | O_NONBLOCK) == -1) {
            if (!(flags & PIPELOG_QUIET)) {
                fprintf(stderr, "*** error: setting input file descriptor to non-blocking: %s\n", strerror(errno));
            }
            use_splice = false;
        }
    }

    int open_flags = use_splice ?
        O_CREAT | O_RDWR | O_CLOEXEC :
        O_CREAT | O_WRONLY | O_CLOEXEC | O_APPEND;

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
                status = PIPELOG_ERROR;
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
                    status = PIPELOG_ERROR;
                    goto cleanup;
                }

                filename = ptr->filename = strdup(buf);
                if (filename == NULL) {
                    if (!(flags & PIPELOG_QUIET)) {
                        fprintf(stderr, "*** error: output[%zu]: cannot allocate string \"%s\": %s\n", init_count, buf, strerror(errno));
                    }
                    status = PIPELOG_ERROR;
                    goto cleanup;
                }
            } else {
                filename = out->filename;
            }

            ptr->fd = open(filename, open_flags, 0644);
            if (ptr->fd < 0 && errno == ENOENT) {
                if (make_parent_dirs(filename, 0755) != 0) {
                    const int errnum = errno;
                    if (!(flags & PIPELOG_QUIET)) {
                        fprintf(stderr, "*** error: output[%zu]: cannot create parent path of \"%s\": %s\n", init_count, filename, strerror(errnum));
                    }
                    status = errnum == EINTR ? PIPELOG_INTERRUPTED : PIPELOG_ERROR;
                    goto cleanup;
                }
                ptr->fd = open(filename, open_flags, 0644);
            }

            if (ptr->fd < 0) {
                const int errnum = errno;
                if (!(flags & PIPELOG_QUIET)) {
                    fprintf(stderr, "*** error: output[%zu]: cannot open file \"%s\": %s\n", init_count, filename, strerror(errnum));
                }
                status = errnum == EINTR ? PIPELOG_INTERRUPTED : PIPELOG_ERROR;
                goto cleanup;
            }

            if (!(open_flags & O_APPEND) && lseek(ptr->fd, 0, SEEK_END) == (off_t)-1) {
                const int errnum = errno;
                if (errnum != EPIPE) {
                    if (!(flags & PIPELOG_QUIET)) {
                        fprintf(stderr, "*** error: output[%zu]: seeking file to end \"%s\": %s\n", init_count, filename, strerror(errno));
                    }
                    status = PIPELOG_ERROR;
                    goto cleanup;
                }
            }

            if (out->link != NULL) {
                if (make_parent_dirs(out->link, 0755) != 0) {
                    const int errnum = errno;
                    if (!(flags & PIPELOG_QUIET)) {
                        fprintf(stderr, "*** error: output[%zu]: cannot create parent path of \"%s\": %s\n", init_count, out->link, strerror(errnum));
                    }
                    status = errnum == EINTR ? PIPELOG_INTERRUPTED : PIPELOG_ERROR;
                    goto cleanup;
                }

                if (unlink(out->link) != 0 && errno != ENOENT) {
                    if (!(flags & PIPELOG_QUIET)) {
                        fprintf(stderr, "*** error: output[%zu]: cannot unlink \"%s\": %s\n", init_count, out->link, strerror(errno));
                    }
                    status = PIPELOG_ERROR;
                    goto cleanup;
                }

                char *absfilename = realpath(filename, link_target);
                if (absfilename == NULL) {
                    const int errnum = errno;
                    if (!(flags & PIPELOG_QUIET)) {
                        fprintf(stderr, "*** error: output[%zu]: cannot get absolute path of \"%s\": %s\n", init_count, filename, strerror(errnum));
                    }
                    status = errnum == EINTR ? PIPELOG_INTERRUPTED : PIPELOG_ERROR;
                    goto cleanup;
                }

                if (symlink(absfilename, out->link) != 0) {
                    const int errnum = errno;
                    if (!(flags & PIPELOG_QUIET)) {
                        fprintf(stderr, "*** error: output[%zu]: cannot create symbolic link at \"%s\": %s\n", init_count, out->link, strerror(errnum));
                    }
                    status = errnum == EINTR ? PIPELOG_INTERRUPTED : PIPELOG_ERROR;
                    goto cleanup;
                }
            }
        } else {
            if (out->fd < 0) {
                if (!(flags & PIPELOG_QUIET)) {
                    fprintf(stderr, "*** error: output[%zu]: illegal file descriptor: %d\n", init_count, out->fd);
                }
                errno = EINVAL;
                status = PIPELOG_ERROR;
                goto cleanup;
            }

            if (out->link != NULL) {
                if (!(flags & PIPELOG_QUIET)) {
                    fprintf(stderr, "*** error: output[%zu]: link has to be NULL if filename is NULL\n", init_count);
                }
                errno = EINVAL;
                status = PIPELOG_ERROR;
                goto cleanup;
            }

            ptr->fd = out->fd;
        }
    }

    if (sigprocmask(SIG_UNBLOCK, &mask, NULL) != 0) {
        if (!(flags & PIPELOG_QUIET)) {
            fprintf(stderr, "*** error: unblocking SIGHUP: %s\n", strerror(errno));
        }
        status = PIPELOG_ERROR;
        goto cleanup;
    }

    for (;;) {
        if (use_splice) {
            if (received_sighup) {
                // re-open all files
                received_sighup = false;
                const time_t now = time(NULL);

                if (localtime_r(&now, &local_now) == NULL) {
                    if (!(flags & PIPELOG_QUIET)) {
                        fprintf(stderr, "*** error: getting local time: %s\n", strerror(errno));
                    }
                    status = PIPELOG_ERROR;
                    goto cleanup;
                }

                int outfd = get_outfd(output, state, 0, &local_now, flags | PIPELOG_FORCE_ROTATE | PIPELOG_BLOCK_SIGHUP | PIPELOG_SPLICE);
                if (outfd < 0 && !(flags & PIPELOG_EXIT_ON_WRITE_ERROR)) {
                    const int errnum = errno;
                    if (!(flags & PIPELOG_QUIET)) {
                        fprintf(stderr, "*** error: writing output: %s\n", strerror(errnum));
                    }
                    status = errnum == EINTR ? PIPELOG_INTERRUPTED : PIPELOG_ERROR;
                    goto cleanup;
                }
            }

            struct pollfd pollfds[] = { { fd, POLLIN, 0 } };
            for (;;) {
                int result = poll(pollfds, 1, -1);

                if (result < 0) {
                    const int errnum = errno;
                    if (errnum == EINTR && received_sighup) {
                        // re-open all files
                        received_sighup = false;
                        const time_t now = time(NULL);

                        if (localtime_r(&now, &local_now) == NULL) {
                            if (!(flags & PIPELOG_QUIET)) {
                                fprintf(stderr, "*** error: getting local time: %s\n", strerror(errno));
                            }
                            status = PIPELOG_ERROR;
                            goto cleanup;
                        }

                        int outfd = get_outfd(output, state, 0, &local_now, flags | PIPELOG_FORCE_ROTATE | PIPELOG_BLOCK_SIGHUP | PIPELOG_SPLICE);
                        if (outfd < 0 && !(flags & PIPELOG_EXIT_ON_WRITE_ERROR)) {
                            const int errnum = errno;
                            if (!(flags & PIPELOG_QUIET)) {
                                fprintf(stderr, "*** error: writing output: %s\n", strerror(errnum));
                            }
                            status = errnum == EINTR ? PIPELOG_INTERRUPTED : PIPELOG_ERROR;
                            goto cleanup;
                        }
                    } else {
                        if (!(flags & PIPELOG_QUIET)) {
                            fprintf(stderr, "*** error: polling input: %s\n", strerror(errnum));
                        }
                        status = errnum == EINTR ? PIPELOG_INTERRUPTED : PIPELOG_ERROR;
                        goto cleanup;
                    }
                } else {
                    break;
                }
            }

            {
                const time_t now = time(NULL);

                if (localtime_r(&now, &local_now) == NULL) {
                    if (!(flags & PIPELOG_QUIET)) {
                        fprintf(stderr, "*** error: getting local time: %s\n", strerror(errno));
                    }
                    status = PIPELOG_ERROR;
                    goto cleanup;
                }
            }

            int outfd = get_outfd(output, state, 0, &local_now, flags | PIPELOG_BLOCK_SIGHUP | PIPELOG_SPLICE);
            if (outfd > -1) {
                for (;;) {
                    const ssize_t wcount = splice(fd, NULL, outfd, NULL, SPLICE_SIZE, SPLICE_F_NONBLOCK);
                    if (wcount < 0) {
                        const int errnum = errno;
                        if (errnum == EINVAL) {
                            if (!(flags & PIPELOG_QUIET)) {
                                fprintf(stderr, "*** error: splice failed, retrying slow path.\n");
                            }
                            use_splice = false;

                            const int infd_flags = fcntl(fd, F_GETFL, 0);
                            if (infd_flags == -1) {
                                if (!(flags & PIPELOG_QUIET)) {
                                    fprintf(stderr, "*** error: getting flags of input file descriptor: %s\n", strerror(errno));
                                }
                            } else if (fcntl(fd, F_SETFL, (infd_flags & ~O_NONBLOCK) | O_APPEND) == -1) {
                                if (!(flags & PIPELOG_QUIET)) {
                                    fprintf(stderr, "*** error: setting input file descriptor to blocking and appending: %s\n", strerror(errno));
                                }
                            }
                            break;
                        } else if (errnum == EINTR && received_sighup) {
                            // re-open all files
                            received_sighup = false;
                            const time_t now = time(NULL);

                            if (localtime_r(&now, &local_now) == NULL) {
                                if (!(flags & PIPELOG_QUIET)) {
                                    fprintf(stderr, "*** error: getting local time: %s\n", strerror(errno));
                                }
                                status = PIPELOG_ERROR;
                                goto cleanup;
                            }
                            outfd = get_outfd(output, state, 0, &local_now, flags | PIPELOG_FORCE_ROTATE | PIPELOG_BLOCK_SIGHUP | PIPELOG_SPLICE);
                            if (outfd < 0) {
                                break;
                            }
                        } else {
                            if (!(flags & PIPELOG_QUIET)) {
                                fprintf(stderr, "*** error: splice failed, retrying slow path: %s\n", strerror(errnum));
                            }
                            status = errnum == EINTR ? PIPELOG_INTERRUPTED : PIPELOG_ERROR;
                            goto cleanup;
                        }
                    } else if (wcount == 0) {
                        goto cleanup;
                    } else {
                        break;
                    }
                }
            } else if (!(flags & PIPELOG_EXIT_ON_WRITE_ERROR)) {
                const int errnum = errno;
                if (!(flags & PIPELOG_QUIET)) {
                    fprintf(stderr, "*** error: writing output: %s\n", strerror(errnum));
                }
                status = errnum == EINTR ? PIPELOG_INTERRUPTED : PIPELOG_ERROR;
                goto cleanup;
            }
        } else { // !use_splice
            unsigned int get_outfd_flags = flags;
            ssize_t rcount = 0;
            if (received_sighup) {
                // pending SIGHUP was delivered when it was unblocked
                get_outfd_flags |= PIPELOG_FORCE_ROTATE;
                received_sighup = false;
            } else {
                rcount = read(fd, buf, sizeof(buf));
                if (rcount == 0) {
                    break;
                }

                if (rcount < 0) {
                    const int errnum = errno;
                    if (errnum == EINTR && received_sighup) {
                        get_outfd_flags |= PIPELOG_FORCE_ROTATE;
                        received_sighup = false;
                    } else {
                        if (!(flags & PIPELOG_QUIET)) {
                            fprintf(stderr, "*** error: reading input: %s\n", strerror(errnum));
                        }
                        status = errnum == EINTR ? PIPELOG_INTERRUPTED : PIPELOG_ERROR;
                        goto cleanup;
                    }
                }
            }

            // defer delivery of SIGHUP until after all log handling
            if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0) {
                if (!(flags & PIPELOG_QUIET)) {
                    fprintf(stderr, "*** error: blocking SIGHUP: %s\n", strerror(errno));
                }
                status = PIPELOG_ERROR;
                goto cleanup;
            }

            if (any_rotate) {
                const time_t now = time(NULL);

                if (localtime_r(&now, &local_now) == NULL) {
                    if (!(flags & PIPELOG_QUIET)) {
                        fprintf(stderr, "*** error: getting local time: %s\n", strerror(errno));
                    }
                    status = PIPELOG_ERROR;
                    goto cleanup;
                }
            }

            for (size_t index = 0; index < count; ++ index) {
                int outfd = get_outfd(output, state, index, &local_now, get_outfd_flags);

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
                                status = PIPELOG_INTERRUPTED;
                                goto cleanup;
                            }

                            if ((flags & PIPELOG_EXIT_ON_WRITE_ERROR)) {
                                status = PIPELOG_ERROR;
                                goto cleanup;
                            }

                            if (errnum != EAGAIN) {
                                state[index].fd = -1;
                            }
                            break;
                        }
                        offset += wcount;
                    }
                } else if (!(flags & PIPELOG_EXIT_ON_WRITE_ERROR)) {
                    const int errnum = errno;
                    if (!(flags & PIPELOG_QUIET)) {
                        fprintf(stderr, "*** error: output[%zu]: writing output: %s\n", index, strerror(errnum));
                    }
                    status = errnum == EINTR ? PIPELOG_INTERRUPTED : PIPELOG_ERROR;
                    goto cleanup;
                }
            }

            if (sigprocmask(SIG_UNBLOCK, &mask, NULL) != 0) {
                if (!(flags & PIPELOG_QUIET)) {
                    fprintf(stderr, "*** error: unblocking SIGHUP: %s\n", strerror(errno));
                }
                status = PIPELOG_ERROR;
                goto cleanup;
            }
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

    if (old_handle_sighup != SIG_ERR && signal(SIGHUP, old_handle_sighup) == SIG_ERR) {
        if (!(flags & PIPELOG_QUIET)) {
            fprintf(stderr, "*** error: signal(SIGHUP, old_handle_sighup): %s\n", strerror(errno));
        }
        status = PIPELOG_ERROR;
        goto cleanup;
    }

    return status;
}
