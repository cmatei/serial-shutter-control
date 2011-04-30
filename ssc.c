#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <netdb.h>
#include <signal.h>
#include <errno.h>

#define FATAL(msg) do { perror(msg); exit(1); } while (0)

static char *serial_port = "/dev/ttyS0";

static unsigned int exp_count = 1;        /* number of exposures */
static unsigned int exp_time  = 1000;     /* exposure time in miliseconds */
static unsigned int exp_pause = 5000;     /* pause between exposures */

static unsigned int phd_dither_size = 0;       /* PHD dither amount (0 = disabled) */
static char *phd_host = "localhost";
static int   phd_port = 4300;
static int   phd_fd = -1;

static char *log_target = NULL;
static struct timeval exp_start, exp_end;

/* MLU method:

   1 - one pulse, cameras that lock the mirror on the short self-timer.
       add mlu_delay to exposure time.

   2 - two pulses, shut_min_pulse len to lock the mirror, observe
       mlu_delay then start exposure */
static unsigned int mlu_method = 2;

static int mlu_delay = 2000;	          /* delay from mirror lockup to exposure */
static int shut_min_pulse = 200;          /* minimum duration for shutter pulse */

static int quiet = 0;                     /* disable progress bar and other output */

static int exposing = 0;

#define VBUF_LEN 1024
static char verbose[VBUF_LEN];

#define ANIMATION_STEP 500
#define ANIMATION_LEN  8

static char animation[ANIMATION_LEN] = { '|', '/', '-', '\\', '|', '/', '-', '\\' };
static int  animation_step = 0;

/* globals ftw! */
static int fd;

static void shutter_fire();
static void shutter_release();

static void sleep_quiet(unsigned int msec);
static void sleep_verbose(unsigned int msec);

static void phd_connect();
static void phd_disconnect();
static void phd_dither();

static void exit_cleanup(int dummy);

static void usage()
{
	fprintf(stderr,
		"\n"
		"Usage: ssc [options] [time in seconds]\n"
		"\n"
		"  -s  serial port to use       [default: %s]\n"
		"  -c  number of exposures      [default: %d]\n"
		"  -t  exposure time in seconds [default: %u]\n"
		"  -p  pause in seconds         [default: %u]\n"
		"  -m  MLU number of pulses     [default: %d]\n"
		"  -M  MLU delay in miliseconds [default: %u]\n"
		"  -S  min shutter in milisec   [default: %u]\n"
		"  -d  PHD dither amount (1..5) [default: %u]\n"
		"  -P  PHD host:port            [default: %s:%d]\n"
		"  -l  log object description"
		"  -q  quiet operation\n"
		"  -h  this help summary\n"
		"\n",
		serial_port, exp_count, exp_time / 1000, exp_pause / 1000,
		mlu_method, mlu_delay, shut_min_pulse,
		phd_dither_size, phd_host, phd_port);
}


static void log_exposure()
{
	FILE *f;
	static char fname[32];
	struct tm *t;
	struct timeval delta;

	if (!log_target)
		return;

	t = gmtime(&exp_start.tv_sec);
	snprintf(fname, 32, "ssc-%4d%02d%02d.log",
		 1900 + t->tm_year, t->tm_mon + 1, t->tm_mday);

	if ((f = fopen(fname, "a")) == NULL)
		return;

	timersub(&exp_end, &exp_start, &delta);

	fprintf(f, "%s, %02d:%02d:%02d, %lu seconds, %lu.%06lu, %lu.%06lu\n",
		log_target,
		t->tm_hour, t->tm_min, t->tm_sec,
		delta.tv_sec + (delta.tv_usec + 500000) / 1000000,
		exp_start.tv_sec, exp_start.tv_usec,
		exp_end.tv_sec, exp_end.tv_usec);

	fclose(f);
}


static void expose(unsigned int msec)
{
	switch (mlu_method) {
	case 1:
		shutter_fire();

		sleep_quiet(mlu_delay);

		gettimeofday(&exp_start, NULL);
		exposing = 1;
		sleep_verbose(exp_time);
		exposing = 0;
		gettimeofday(&exp_end, NULL);

		shutter_release();
		break;

	case 2:
		shutter_fire();
		sleep_quiet(shut_min_pulse);
		shutter_release();

		sleep_quiet(mlu_delay);

		shutter_fire();

		gettimeofday(&exp_start, NULL);
		exposing = 1;
		sleep_verbose(exp_time);
		exposing = 0;
		gettimeofday(&exp_end, NULL);

		shutter_release();
		break;
	default:
		printf("unknown MLU method %d\n", mlu_method);
	}
}

int main(int argc, char **argv)
{
	unsigned int i;
	int c;
	char *p;

	verbose[0] = 0;

	while ((c = getopt(argc, argv, "c:t:p:m:M:S:qhP:d:l:")) != -1) {
		switch (c) {
		case 's':
			serial_port = strdup(optarg);
			break;
		case 'c':
			exp_count = atoi(optarg);
			break;
		case 't':
			exp_time = atoi(optarg) * 1000;
			break;
		case 'p':
			exp_pause = atoi(optarg) * 1000;
			break;
		case 'm':
			mlu_method = atoi(optarg);
			break;
		case 'M':
			mlu_delay = atoi(optarg) * 1000;
			break;
		case 'S':
			/* this is in msec */
			shut_min_pulse = atoi(optarg);
			break;
		case 'q':
			quiet = 1;
			break;
		case 'd':
			phd_dither_size = atoi(optarg);
			if (phd_dither_size > 5) { usage(); exit(0); }
			break;

		case 'P':
			phd_host = strdup(optarg);
			if ((p = strchr(phd_host, ':'))) {
				*p++ = 0;
				phd_port = atoi(p);
			}
			break;

		case 'l':
			log_target = strdup(optarg);
			break;

		case 'h':
		default:
			usage();
			exit(0);
		}
	}

	if (optind < argc)
		exp_time = atoi(argv[optind]) * 1000;

	if ((fd = open(serial_port, O_RDWR)) < 0)
		FATAL("open");

	signal(SIGINT, exit_cleanup);

	shutter_release();

	phd_connect();

	for (i = 1; i <= exp_count; i++) {
		if (i != 1 && phd_dither_size) {
			printf("Dithering...\r");
			fflush(stdout);

			phd_dither();
		}

		snprintf(verbose, VBUF_LEN, "Exposure %u/%u", i, exp_count);

		if (i != 1)
			sleep_quiet(exp_pause);

		expose(exp_time);

		log_exposure();
	}

	phd_disconnect();

	return 0;
}


static void set_rts(int on)
{
	int cbits;

	ioctl(fd, TIOCMGET, &cbits);
	if (on)
		cbits |= TIOCM_RTS;
	else
		cbits &= ~TIOCM_RTS;

	ioctl(fd, TIOCMSET, &cbits);
}

static void shutter_fire()
{
	set_rts(1);
}

static void shutter_release()
{
	set_rts(0);
}

static void exit_cleanup(int dummy)
{
	shutter_release();

	if (exposing) {
		exposing = 0;
		gettimeofday(&exp_end, NULL);
		log_exposure();
	}
	close(fd);
	exit(0);
}

static void sleep_quiet(unsigned int msec)
{
	struct timespec t, rem;
	int r;

	t.tv_sec  = msec / 1000;
	t.tv_nsec = (msec - msec / 1000 * 1000) * 1000000;

again:
	r = nanosleep(&t, &rem);
	if ((r == -1) && (errno == EINTR)) {
		t = rem;
		goto again;
	}
}

static void sleep_verbose(unsigned int msec)
{
	unsigned int sofar;

	if (quiet) {
		sleep_quiet(msec);
		return;
	}

	sofar = 0;

	while (sofar + ANIMATION_STEP <= msec) {
		printf("%s   %3d%% %c\r", verbose,
		       100 * sofar / msec, animation[animation_step++]);
		fflush(stdout);

		animation_step %= ANIMATION_LEN;

		sleep_quiet(ANIMATION_STEP);
		sofar += ANIMATION_STEP;
	}

	sleep_quiet(msec - sofar);

	printf("%s ... done   \n", verbose);
	fflush(stdout);
}

static void phd_connect()
{
	struct sockaddr_in saddr;
	struct hostent *he;

	if (!phd_dither_size)
	       return;

	if ((phd_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
		FATAL("socket");

	if ((he = gethostbyname(phd_host)) == NULL)
		FATAL("gethostbyname");

	memset(&saddr, 0, sizeof(struct sockaddr_in));
	saddr.sin_family = AF_INET;
	saddr.sin_addr.s_addr = *(in_addr_t *) he->h_addr;
	saddr.sin_port = htons(phd_port);

	if (connect(phd_fd, (struct sockaddr *) &saddr, sizeof(struct sockaddr_in)) == -1)
		FATAL("connect");
}

static void phd_disconnect()
{
	if (phd_fd != -1)
		close(phd_fd);
}

static void phd_dither()
{
	static unsigned char phd_cmds[5] = { 3, 4, 5, 12, 13 };
	unsigned char c;

	if (write(phd_fd, &phd_cmds[phd_dither_size], 1) != 1)
		FATAL("write");

	if (read(phd_fd, &c, 1) != 1)
		FATAL("read");
}
