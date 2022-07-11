#include "pipelog.h"

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>

enum {
    OPT_HELP,
    OPT_VERSION,
    OPT_PIDFILE,
    OPT_FIFO,
    OPT_QUIET,
    OPT_EXIT_ON_WRITE_ERROR,
    OPT_NO_SPLICE,
    OPT_COUNT,
};

static const struct option options[] = {
    [OPT_HELP]                = { "help",                no_argument,       0, 'h' },
    [OPT_VERSION]             = { "version",             no_argument,       0, 'v' },
    [OPT_PIDFILE]             = { "pidfile",             required_argument, 0, 'p' },
    [OPT_FIFO]                = { "fifo",                required_argument, 0, 'f' },
    [OPT_QUIET]               = { "quiet",               no_argument,       0, 'q' },
    [OPT_EXIT_ON_WRITE_ERROR] = { "exit-on-write-error", no_argument,       0, 'e' },
    [OPT_NO_SPLICE]           = { "no-splice",           no_argument,       0, 'S' },
    [OPT_COUNT]               = { 0, 0, 0, 0 },
};

static volatile bool reveiced_sigint = false;

static void handle_sigint(int sig) {
    reveiced_sigint = true;
    if (isatty(STDERR_FILENO)) {
        fprintf(stderr, "\n");
    }
}

static void short_usage(int argc, char *argv[]) {
    const char *progname = argc > 0 ? argv[0] : "pipelog";
    printf(
        "Usage: %s [OPTION]... [--] [FILE [@LINK]]...\n",
        progname
    );
}

static void usage(int argc, char *argv[]) {
    const char *progname = argc > 0 ? argv[0] : "pipelog";
    short_usage(argc, argv);
    printf(
        "pipe to log rotated files\n"
        "\n"
        "\n"
        "FILE can be a path of to file or \"STDOUT\" or \"STDERR\". \"-\" is a shorthand\n"
        "for \"STDOUT\".\n"
        "If FILE is a path it may contain strftime compatible format specifications.\n"
        "If any of the log file's anchestor directories don't exists they are created.\n"
        "The directory names may also contain format specifications.\n"
        "\n"
        "LINK may be a path where a symbolic link to the latest FILE is created.\n"
        "Note that the target of the link will be the absolute path of FILE.\n"
        "This is of course only possible when FILE is a path.\n"
        "\n"
        "If SIGHUP is sent to pipelog it re-opens all it's open files. This may lead\n"
        "the creation of new empty log files if the timestamp changed.\n"
        "\n"
        "If there is only one output file splice() is used to transfer data without\n"
        "user space copies.\n"
        "\n"
        "\n"
        "OPTIONS:\n"
        "    -h, --help                 Print this help message.\n"
        "    -v, --version              Print version.\n"
        "    -p, --pidfile=FILE         Write pipelog's process ID to FILE.\n"
        "                               Send SIGINT or SIGTERM to this process ID for\n"
        "                               graceful shutdown before the input stream\n"
        "                               ended.\n"
        "    -f, --fifo=FILE            Read input from FILE, create FILE as fifo if\n"
        "                               not exists and re-open file when at end.\n"
        "    -q, --quiet                Don't print error messages.\n"
        "    -e, --exit-on-write-error  Exit if writing to any output fails or when\n"
        "                               opening log files on log rotate fails.\n"
        "    -S, --no-splice            Don't try to use splice() system call in case\n"
        "                               there is only one output file.\n"
        "\n"
        "\n"
        "EXAMPLE:\n"
        "\n"
        "    while [ : ]; do\n"
        "        echo \"[$(date +'%%Y-%%m-%%d %%T%%z')] some log\"\n"
        "        sleep 1\n"
        "    done | %s - \\\n"
        "        /var/log/myservice-%%Y-%%m-%%d.log \\\n"
        "        @/var/log/myservice.log\n"
        "\n"
        "\n"
        "https://github.com/panzi/pipelog\n"
        "(c) 2022 Mathias Panzenböck\n",
        progname
    );
}

int main(int argc, char *argv[]) {
    int flags = PIPELOG_NONE;
    int longind = 0;
    const char *pidfile = NULL;
    const char *fifo = NULL;

    for (;;) {
        int opt = getopt_long(argc, argv, "hvpqeS", options, &longind);

        if (opt == -1) {
            break;
        }

        switch (opt) {
            case 0:
                // switch (longind) {
                // }
                break;

            case 'h':
                usage(argc, argv);
                return 0;

            case 'v':
                printf("%d.%d.%d\n", PIPELOG_VERSION_MAJOR, PIPELOG_VERSION_MINOR, PIPELOG_VERSION_PATCH);
                return 0;

            case 'p':
                pidfile = optarg;
                break;

            case 'q':
                flags |= PIPELOG_QUIET;
                break;

            case 'e':
                flags |= PIPELOG_EXIT_ON_WRITE_ERROR;
                break;

            case 'S':
                flags |= PIPELOG_NO_SPLICE;
                break;

            case 'f':
                fifo = optarg;
                break;

            case '?':
                short_usage(argc, argv);
                return 1;

            default:
                assert(false);
        }
    }

    if (argc == optind) {
        fprintf(stderr, "*** error: illegal number of arguments\n");
        short_usage(argc, argv);
        return 1;
    }

    // Set stdout and stderr to be line buffered so that when this
    // program wirtes a log message itself it has a smaller chance of
    // interfering with the actual log messages (if those shall be
    // written to stdout/stderr).
    // This is ok since log messages as written here are always single
    // and whole lines (assuming strerror(errno) never returns a
    // multiline string).
    if (setvbuf(stdout, NULL, _IOLBF, 0) != 0) {
        perror("*** error: setvbuf(stderr, NULL, _IOLBF, 0)");
        return 1;
    }

    if (setvbuf(stderr, NULL, _IOLBF, 0) != 0) {
        perror("*** error: setvbuf(stderr, NULL, _IOLBF, 0)");
        return 1;
    }

    size_t count = 0;
    for (int index = optind; index < argc; ++ index) {
        const char *arg = argv[index];
        if (*arg == 0) {
            fprintf(stderr, "*** error: FILE may not be an empty string\n");
            return 1;
        } else if (strcmp(arg, "STDOUT") == 0 || strcmp(arg, "-") == 0 || strcmp(arg, "STDERR") == 0) {
            if (index + 1 < argc && argv[index + 1][0] == '@') {
                fprintf(stderr, "*** error: Only if FILE is a path it may be followed by @LINK\n");
                return 1;
            }
        } else if (index + 1 < argc && argv[index + 1][0] == '@') {
            ++ index;
            if (argv[index][1] == 0) {
                fprintf(stderr, "*** error: LINK may not be an empty string\n");
                return 1;
            }
        }
        ++ count;
    }

    if (signal(SIGINT, handle_sigint) == SIG_ERR) {
        if (!(flags & PIPELOG_QUIET)) {
            fprintf(stderr, "*** error: signal(SIGINT, handle_sigint): %s\n", strerror(errno));
        }
        return 1;
    }

    if (signal(SIGTERM, handle_sigint) == SIG_ERR) {
        if (!(flags & PIPELOG_QUIET)) {
            fprintf(stderr, "*** error: signal(SIGTERM, handle_sigint): %s\n", strerror(errno));
        }
        return 1;
    }

    if (pidfile != NULL) {
        if (make_parent_dirs(pidfile, 755) != 0) {
            if (!(flags & PIPELOG_QUIET)) {
                fprintf(stderr, "*** error: creating parent directories of pidfile \"%s\": %s\n", pidfile, strerror(errno));
            }
            return 1;
        }

        FILE *fp = fopen(pidfile, "wx");
        if (fp == NULL) {
            if (!(flags & PIPELOG_QUIET)) {
                fprintf(stderr, "*** error: opening pidfile \"%s\": %s\n", pidfile, strerror(errno));
            }
            return 1;
        }
        const pid_t pid = getpid();
        fprintf(fp, "%d\n", pid);

        fclose(fp);
    }

    struct Pipelog_Output *output = calloc(count, sizeof(struct Pipelog_Output));
    if (output == NULL) {
        if (!(flags & PIPELOG_QUIET)) {
            fprintf(stderr, "*** error: allocating memory: %s\n", strerror(errno));
        }
        return 1;
    }

    for (size_t index = 0; index < count; ++ index) {
        const size_t argind = optind + index;
        const char *arg = argv[argind];
        if (strcmp(arg, "STDOUT") == 0 || strcmp(arg, "-") == 0) {
            output[index] = (struct Pipelog_Output){
                .fd       = STDOUT_FILENO,
                .filename = NULL,
                .link     = NULL,
            };
        } else if (strcmp(arg, "STDERR") == 0) {
            output[index] = (struct Pipelog_Output){
                .fd       = STDERR_FILENO,
                .filename = NULL,
                .link     = NULL,
            };
        } else {
            const char *link = NULL;
            if (argind + 1 < argc && argv[argind + 1][0] == '@') {
                link = argv[argind + 1] + 1;
                ++ optind;
            }
            output[index] = (struct Pipelog_Output){
                .fd       = -1,
                .filename = arg,
                .link     = link,
            };
        }
    }

    int status = PIPELOG_SUCCESS;

    if (fifo == NULL) {
        status = pipelog(STDIN_FILENO, output, count, flags);
    } else {
        if (make_parent_dirs(fifo, 0755) != 0) {
            const int errnum = errno;
            if (!(flags & PIPELOG_QUIET)) {
                fprintf(stderr, "*** error: cannot create parent path of \"%s\": %s\n", fifo, strerror(errnum));
            }
            status = errnum == EINTR ? PIPELOG_INTERRUPTED : PIPELOG_ERROR;
            goto cleanup;
        }

        if (mkfifo(fifo, 0644) != 0) {
            const int errnum = errno;
            if (errnum == EEXIST) {
                struct stat meta;

                if (stat(fifo, &meta) != 0) {
                    const int errnum = errno;
                    if (!(flags & PIPELOG_QUIET)) {
                        fprintf(stderr, "*** error: cannot access fifo \"%s\": %s\n", fifo, strerror(errnum));
                    }
                    status = errnum == EINTR ? PIPELOG_INTERRUPTED : PIPELOG_ERROR;
                    goto cleanup;
                }

                if ((meta.st_mode & S_IFMT) != S_IFIFO) {
                    if (!(flags & PIPELOG_QUIET)) {
                        fprintf(stderr, "*** error: file exists but isn't fifo \"%s\": %s\n", fifo, strerror(errno));
                    }
                    status = PIPELOG_ERROR;
                    goto cleanup;
                }
            } else {
                if (!(flags & PIPELOG_QUIET)) {
                    fprintf(stderr, "*** error: creating fifo \"%s\": %s\n", fifo, strerror(errnum));
                }
                status = errnum == EINTR ? PIPELOG_INTERRUPTED : PIPELOG_ERROR;
                goto cleanup;
            }
        }

        while (!reveiced_sigint) {
            int fd = open(fifo, O_RDONLY | O_CLOEXEC | O_NONBLOCK);

            if (fd < 0) {
                const int errnum = errno;
                if (!(flags & PIPELOG_QUIET)) {
                    fprintf(stderr, "*** error: opening fifo \"%s\": %s\n", fifo, strerror(errnum));
                }
                status = errnum == EINTR ? PIPELOG_INTERRUPTED : PIPELOG_ERROR;
                break;
            }

            status = pipelog(fd, output, count, flags);

            if (close(fd) != 0) {
                const int errnum = errno;
                if (!(flags & PIPELOG_QUIET)) {
                    fprintf(stderr, "*** error: closing fifo \"%s\": %s\n", fifo, strerror(errnum));
                }
                if (status == PIPELOG_SUCCESS) {
                    status = errnum == EINTR ? PIPELOG_INTERRUPTED : PIPELOG_ERROR;
                }
                break;
            }
        }

        if (unlink(fifo) != 0) {
            const int errnum = errno;
            if (!(flags & PIPELOG_QUIET)) {
                fprintf(stderr, "*** error: removing fifo \"%s\": %s\n", fifo, strerror(errnum));
            }
            if (status == PIPELOG_SUCCESS) {
                status = errnum == EINTR ? PIPELOG_INTERRUPTED : PIPELOG_ERROR;
            }
        }
    }

cleanup:
    free(output);

    if (pidfile != NULL && unlink(pidfile) != 0) {
        const int errnum = errno;
        if (!(flags & PIPELOG_QUIET)) {
            fprintf(stderr, "*** error: removing pidfile \"%s\": %s\n", pidfile, strerror(errnum));
        }
        if (status == PIPELOG_SUCCESS) {
            status = errnum == EINTR ? PIPELOG_INTERRUPTED : PIPELOG_ERROR;
        }
    }

    return status == PIPELOG_INTERRUPTED && reveiced_sigint ? PIPELOG_SUCCESS : status;
}
