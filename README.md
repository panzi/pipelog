pipelog - pipe to log rotated files
===================================

[![License](https://img.shields.io/github/license/panzi/pipelog)](https://github.com/panzi/pipelog/blob/main/LICENSE)

A simple program that redirects its standard input to multiple output files.
The path of the output files may include `strftime()` compatible format
specifiers causing an automated log rotation.

```plain
Usage: pipelog [OPTION]... [--] [FILE [@LINK]]...
pipe to log rotated files


FILE can be a path of to file or "STDOUT" or "STDERR". "-" is a shorthand
for "STDOUT".
If FILE is a path it may contain strftime compatible format specifications.
If any of the log file's anchestor directories don't exists they are created.
The directory names may also contain format specifications.

LINK may be a path where a symbolic link to the latest FILE is created.
Note that the target of the link will be the absolute path of FILE.
This is of course only possible when FILE is a path.

If SIGHUP is sent to pipelog it re-opens all it's open files. This may lead
the creation of new empty log files if the timestamp changed.

If there is only one output file splice() is used to transfer data without
user space copies.


OPTIONS:
    -h, --help                 Print this help message.
    -v, --version              Print version.
    -p, --pidfile=FILE         Write pipelog's process ID to FILE.
                               Send SIGINT or SIGTERM to this process ID for
                               graceful shutdown before the input stream
                               ended.
    -f, --fifo=FILE            Read input from FILE, create FILE as fifo if
                               not exists and re-open file when at end.
    -q, --quiet                Don't print error messages.
    -e, --exit-on-write-error  Exit if writing to any output fails or when
                               opening log files on log rotate fails.
    -S, --no-splice            Don't try to use splice() system call in case
                               there is only one output file.


EXAMPLE:

    while [ : ]; do
        echo "[$(date +'%Y-%m-%d %T%z')] some log"
        sleep 1
    done | pipelog - \
        /var/log/myservice-%Y-%m-%d.log \
        @/var/log/myservice.log


https://github.com/panzi/pipelog
(c) 2022 Mathias Panzenb??ck
```
