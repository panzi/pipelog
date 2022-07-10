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


OPTIONS:
    -h, --help                 Print this help message
    -q, --quiet                Don't print error messages
    -e, --exit-on-write-error  Exits if writing to any output fails or when
                               opening log files on log rotate fails.


EXAMPLE:

    while [ : ]; do
        echo "[$(date +'%Y-%m-%d %T%z')] some log"
        sleep 1
    done | pipelog - \
        /var/log/myservice-%Y-%m-%d.log \
        @/var/log/myservice.log


https://github.com/panzi/pipelog
(c) 2022 Mathias Panzenböck
```
