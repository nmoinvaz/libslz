/*
 * Copyright (C) 2013-2015 Willy Tarreau <w@1wt.eu>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#define _GNU_SOURCE /* for F_SETPIPE_SZ */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#include <basetsd.h>
#else
#include <unistd.h>
#include <sys/mman.h>
#include <sys/user.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "slz.h"

#ifdef _WIN32
#define isatty _isatty
#define open _open
#define ssize_t SSIZE_T
#define off_t SSIZE_T
#define NORETURN
#else
#define NORETURN __attribute__((noreturn))
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
#  define SET_BINARY_MODE(file) setmode(fileno(file), O_BINARY)
#else
#  define SET_BINARY_MODE(file)
#endif

/* display the message and exit with the code */
NORETURN void die(int code, const char *format, ...)
{
        va_list args;

        va_start(args, format);
        vfprintf(stderr, format, args);
        va_end(args);
        exit(code);
}

NORETURN void usage(const char *name, int code)
{
	die(code,
	    "Usage: %s [option]* [file]\n"
	    "\n"
	    "The following arguments are supported :\n"
	    "  -0         disable compression, only uses output format\n"
	    "  -1         compress faster\n"
	    "  -2         compress better\n"
	    "  -3 .. -9   compress even better [default]\n"
	    "  -b <size>  only use <size> bytes from the input file\n"
	    "  -c         send output to stdout [default]\n"
	    "  -f         force sending output to a terminal\n"
	    "  -h         display this help\n"
	    "  -l <loops> loop <loops> times over the same file\n"
	    "  -n         does nothing, just for gzip compatibility\n"
	    "  -t         test mode: do not emit anything\n"
	    "  -v         increase verbosity\n"
	    "\n"
	    "  -D         use raw Deflate output format (RFC1951)\n"
	    "  -G         use Gzip output format (RFC1952) [default]\n"
	    "  -Z         use Zlib output format (RFC1950)\n"
	    "\n"
	    "If no file is specified, stdin will be used instead.\n"
	    "\n"
	    ,name);
}


int main(int argc, char **argv)
{
	const char *name = argv[0];
	struct stat instat;
	struct slz_stream strm;
	unsigned char *outbuf;
	unsigned char *buffer;
	off_t toread = -1;
	size_t outblen;
	size_t outbsize;
	size_t block_size;
	off_t totin = 0;
	off_t totout = 0;
	int loops = 1;
	int console = 1;
	int level   = 3;
	int verbose = 0;
	int test    = 0;
	int format  = SLZ_FMT_GZIP;
	int force   = 0;
	int fd = 0;
	int error = 0;

    SET_BINARY_MODE(stdin);
    SET_BINARY_MODE(stdout);

	argv++;
	argc--;

	while (argc > 0) {
		if (**argv != '-')
			break;

		if (argv[0][0] == '-' && argv[0][1] >= '0' && argv[0][1] <= '9')
			level = argv[0][1] - '0';

		else if (strcmp(argv[0], "-b") == 0) {
			if (argc < 2)
				usage(name, 1);
			toread = atoll(argv[1]);
			argv++;
			argc--;
		}

		else if (strcmp(argv[0], "-c") == 0)
			console = 1;

		else if (strcmp(argv[0], "-f") == 0)
			force = 1;

		else if (strcmp(argv[0], "-h") == 0)
			usage(name, 0);

		else if (strcmp(argv[0], "-l") == 0) {
			if (argc < 2)
				usage(name, 1);
			loops = atoi(argv[1]);
			argv++;
			argc--;
		}

		else if (strcmp(argv[0], "-n") == 0)
			/* just for gzip compatibility */ ;

		else if (strcmp(argv[0], "-t") == 0)
			test = 1;

		else if (strcmp(argv[0], "-v") == 0)
			verbose++;

		else if (strcmp(argv[0], "-D") == 0)
			format = SLZ_FMT_DEFLATE;

		else if (strcmp(argv[0], "-G") == 0)
			format = SLZ_FMT_GZIP;

		else if (strcmp(argv[0], "-Z") == 0)
			format = SLZ_FMT_ZLIB;

		else
			usage(name, 1);

		argv++;
		argc--;
	}

	if (argc > 0) {
		fd = open(argv[0], _O_RDONLY | _O_BINARY);
		if (fd == -1) {
			perror("open()");
			exit(1);
		}
	}

	if (isatty(1) && !test && !force)
		die(1, "Use -f if you really want to send compressed data to a terminal, or -h for help.\n");

	slz_make_crc_table();
	slz_prepare_dist_table();

	block_size = 32768;
	if (level > 1)
		block_size *= 4; // 128 kB
	if (level > 2)
		block_size *= 8; // 1 MB

	outbsize = 2 * block_size; // allows to pack more than one full output at each round
	outbuf = calloc(1, outbsize + 4096);
	if (!outbuf) {
		perror("calloc");
		exit(1);
	}

	/* principle : we'll determine the input file size and try to map the
	 * file at once. If it works we have a single input buffer of the whole
	 * file size. If it fails we'll bound the input buffer to the buffer size
	 * and read the input in smaller blocks.
	 */
	if (toread < 0) {
		if (fstat(fd, &instat) == -1) {
			perror("fstat(fd)");
			exit(1);
		}
		toread = instat.st_size;
	}

	buffer = calloc(1, block_size);
	if (!buffer) {
		perror("calloc");
		exit(1);
	}

	while (loops--) {
		slz_init(&strm, !!level, format);
		size_t count = block_size;
		int more = 1;
		outblen = 0;
		do {
			if (toread < (uint32_t)block_size) {
				count = (size_t)toread;
				more = 0;
			}

			ssize_t ret = read(fd, buffer, count);
			if (ret < 0) {
				perror("read");
				exit(2);
			}

			toread -= ret;
			totin += ret;

			outblen += slz_encode(&strm, outbuf + outblen, buffer, ret, more);
			if (outblen + block_size > outbsize) {
				/* not enough space left, need to flush */
				if (console && !test && !error)
					if (write(1, outbuf, outblen) < 0)
						error = 1;
				totout += outblen;
				outblen = 0;
			}
		} while (more);

		outblen += slz_finish(&strm, outbuf + outblen);
		totout += outblen;
		if (console && !test && !error)
			if (write(1, outbuf, outblen) < 0)
				error = 1;
	}
	if (verbose)
		fprintf(stderr, "totin=%llu totout=%llu ratio=%.2f%% crc32=%08x\n",
		        totin, totout, totout * 100.0 / totin, strm.crc32);

	return error;
}
