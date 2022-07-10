#include "pipelog.h"

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>

enum {
    OPT_HELP,
    OPT_PID,
    OPT_QUIET,
    OPT_EXIT_ON_WRITE_ERROR,
    OPT_VERSION,
    OPT_COUNT,
};

static const struct option options[] = {
    [OPT_HELP]                = { "help",                no_argument,       0, 'h' },
    [OPT_PID]                 = { "pidfile",             required_argument, 0, 'p' },
    [OPT_QUIET]               = { "quiet",               no_argument,       0, 'q' },
    [OPT_EXIT_ON_WRITE_ERROR] = { "exit-on-write-error", no_argument,       0, 'e' },
    [OPT_VERSION]             = { "version",             no_argument,       0, 'v' },
    [OPT_COUNT]               = { 0, 0, 0, 0 },
};

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
        "\n"
        "OPTIONS:\n"
        "    -h, --help                 Print this help message.\n"
        "    -v, --version              Print version.\n"
        "    -p, --pidfile=FILE         Write pipelog's process ID to FILE.\n"
        "    -q, --quiet                Don't print error messages.\n"
        "    -e, --exit-on-write-error  Exit if writing to any output fails or when\n"
        "                               opening log files on log rotate fails.\n"
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

    for (;;) {
        int opt = getopt_long(argc, argv, "hqev", options, &longind);

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

    const int status = pipelog(STDIN_FILENO, output, count, pidfile, flags);

    free(output);

    return status;
}
