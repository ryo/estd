/*
 * Enhanced Speedstep & PowerNow Daemon for NetBSD & DragonFly BSD, Code (c) 
 * 2004-2007 by Ove Soerensen, Portions (c) 2006,2009 Johannes Hofmann, 2007
 * Stephen M. Rumble
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer. 2.
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS 'AS IS' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE   FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */


#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#if defined(__DragonFly__)
 #include <kinfo.h>
 #include <libutil.h>
#else
 #include <sys/sched.h>
 #include <util.h>
#endif
#include <errno.h>
#include <signal.h>

#define ESTD_VERSION "Release 11"
#define BATTERY 0
#define SMOOTH 1
#define AGGRESSIVE 2
#define DEF_POLL 500000
#define MIN_POLL 10000
#define DEF_HIGH 80
#define DEF_LOW 40
#define TECH_UNKNOWN 0
#define TECH_EST 1
#define TECH_POWERNOW 2
#define TECH_ACPI 3
#define TECH_INTREPID 4
#define TECH_LOONGSON 5
#define TECH_ROCKCHIP 6
#define TECH_GENERIC 7
#define TECH_MAX 7
 
/* this is ugly, but... <shrug> */
#define MAX_FREQS 32
#define SYSCTLBUF 255
#define MAX_CPUS  128

extern char    *optarg;
extern int      optind;

#if defined(__DragonFly__)
 #define useconds_t unsigned int
#endif

/* command-line options */
int             daemonize = 0;
int             verbose = 0;
int             nicemod = 0;
int             strategy = SMOOTH;
useconds_t      poll = DEF_POLL;
int             high = DEF_HIGH;
int             low = DEF_LOW;
useconds_t      lowgrace = 0;
int             minmhz = 0;
int             maxmhz = INT_MAX;
int             listfreq = 0;
int             tech = TECH_UNKNOWN;
int             use_clockmod = 0;
int             clockmod_min = -1;
int             clockmod_max = -1;

/* a domain is a set of CPUs for which the frequency must be set together */
struct domain {
	int         *cpus;
	int          ncpus;
	char        *freqctl;
	char        *setctl;
	useconds_t   lowtime;
	int          freqtab[MAX_FREQS];
	int          nfreqs;
	int          minidx;
	int          maxidx;
	int          curcpu;
	int          curfreq;
};

int             ncpus = 0;
struct domain  *domain;
int             ndomains;

#if defined(__DragonFly__)
 static struct kinfo_cputime *cp_time;
 static struct kinfo_cputime *cp_old;
 size_t cp_time_len;
#else
 static int cpumib[2] = {CTL_KERN, KERN_CP_TIME};
 static u_int64_t cp_time[MAX_CPUS][CPUSTATES];
 static u_int64_t cp_old[MAX_CPUS][CPUSTATES];
 static u_int64_t cp_diff[MAX_CPUS][CPUSTATES];
 static const size_t cp_time_max_size = sizeof(cp_time[0]) * MAX_CPUS;
#endif

static char	*techdesc[TECH_MAX + 1] = {"Unknown",
				"Enhanced SpeedStep",
				"PowerNow",
				"ACPI P-States",
				"Intrepid",
				"Loongson",
				"Rockchip",
				"Generic"
				};
static char	*freqctl[TECH_MAX + 1] = {	"",	
				"machdep.est.frequency.available",
				"machdep.powernow.frequency.available",
				"hw.acpi.cpu.px_dom0.available",
				"machdep.intrepid.frequency.available",
				"machdep.loongson.frequency.available",
				"machdep.cpu.frequency.available",
				"machdep.frequency.available"
				};
static char	*setctl[TECH_MAX + 1] = {	"",
				"machdep.est.frequency.target",
				"machdep.powernow.frequency.target",
				"hw.acpi.cpu.px_dom0.select",
				"machdep.intrepid.frequency.target",
				"machdep.loongson.frequency.target",
				"machdep.cpu.frequency.target",
				"machdep.frequency.current"
				};

void
usage()
{
	printf("usage: estd [-d] [-o] [-n] [-A] [-C] [-E] [-I] [-L] [-R] [-P] [-G] [-a] [-s] [-b] [-p poll interval in us] [-g grace period] [-l low watermark percentage] [-h high watermark percentage] [-m minimum MHz] [-M maximum MHz]\n");
	printf("       estd -v\n");
	printf("       estd -f\n");
	exit(1);
}

void
version()
{
	printf("estd ");
	printf(ESTD_VERSION);
	printf(" Copyright (c) 2004-2009 Ove Soerensen\n");
	printf("Details at http://www.ecademix.com/JohannesHofmann/estd.html - Contact via Johannes.Hofmann@gmx.de\n");
	exit(0);
}

void *
ecalloc(size_t number, size_t size) {
	void *ret = calloc(number, size);
	if (ret == NULL) {
		fprintf(stderr, "estd: calloc failed (errno %d)\n", errno);
		exit(1);
	}

	return ret;
}

int
acpi_init_domain(int d)
{
	char name[256];
	char members[SYSCTLBUF];
	char *mp;
	size_t memberssize = SYSCTLBUF;
	int i;

	snprintf(name, sizeof(name), "hw.acpi.cpu.px_dom%d.members", d);
	if (sysctlbyname(name, &members, &memberssize, NULL, 0) < 0)
		return 1;

	ndomains = d + 1;
	domain = realloc(domain, ndomains * sizeof(struct domain));
	if (domain == NULL) {
		fprintf(stderr, "estd: realloc failed (errno %d)\n", errno);
		exit(1);
	}
	memset(&domain[d], 0, sizeof(struct domain));

	domain[d].ncpus = 0;
	domain[d].cpus = ecalloc(ncpus, sizeof(int));
	mp = members;
	while ((mp = strstr(mp, "cpu"))) {
		mp += strlen("cpu");
		domain[d].cpus[domain[d].ncpus++] = strtol(mp, &mp, 10);
	}

	asprintf(&domain[d].freqctl, "hw.acpi.cpu.px_dom%d.available", d);
	asprintf(&domain[d].setctl, "hw.acpi.cpu.px_dom%d.select", d);
	if (domain[d].setctl == NULL || domain[d].freqctl == NULL) {
		fprintf(stderr, "estd: asprintf failed\n");
		exit(1);
	}

	if ((!daemonize) && (verbose))
		for (i = 0; i < domain[d].ncpus; i++)
			printf("estd: domain %d: member %d\n", d, domain[d].cpus[i]);

	return 0;
}

int
acpi_init()
{
	int d = 0;

	while (acpi_init_domain(d) == 0)
		d++;

	return d > 0 ? 0 : 1;
}


/* returns cpu-usage in percent, mean over the sleep-interval or -1 if an error occured */
#if defined(__DragonFly__)
int
get_cputime()
{
	size_t len = cp_time_len;

	memcpy(cp_old, cp_time, cp_time_len);

	if (sysctlbyname("kern.cputime", cp_time, &len, NULL, 0) < 0) {
		fprintf(stderr, "estd: Cannot get CPU status\n");
		exit(1);	
	}

	return 0;
}

/* get maximum load of a cpu in domain d */
int
get_cpuusage(int d)
{
	int                  i, cpu, load, max_load = 0;
	u_int64_t            total_time;
	struct kinfo_cputime cp_diff;

	for (i = 0; i < domain[d].ncpus; i++) {
		cpu = domain[d].cpus[i];
		total_time = 0;

		cp_diff.cp_user = cp_time[cpu].cp_user - cp_old[cpu].cp_user;
		total_time += cp_diff.cp_user;
		cp_diff.cp_nice = cp_time[cpu].cp_nice - cp_old[cpu].cp_nice;
		total_time += cp_diff.cp_nice;
		cp_diff.cp_sys = cp_time[cpu].cp_sys - cp_old[cpu].cp_sys;
		total_time += cp_diff.cp_sys;
		cp_diff.cp_intr = cp_time[cpu].cp_intr - cp_old[cpu].cp_intr;
		total_time += cp_diff.cp_intr;
		cp_diff.cp_idle = cp_time[cpu].cp_idle - cp_old[cpu].cp_idle;
		total_time += cp_diff.cp_idle;

		if (total_time > 0) {
			load = 100 - ((cp_diff.cp_idle + (cp_diff.cp_nice * nicemod)) * 100) / total_time;
			if (load > max_load)
				max_load = load;
		}
	}

	return max_load;
}
#else

int
get_cputime(void)
{
	size_t cp_time_size = cp_time_max_size;

	memcpy(cp_old, cp_time, cp_time_max_size);
	if (sysctl(cpumib, 2, &cp_time, &cp_time_size, NULL, 0) < 0) {
		fprintf(stderr, "estd: Cannot get CPU status\n");
		exit(1);
	}

	ncpus = cp_time_size / sizeof cp_time[0];

	return 0;
}

int
get_cpuusage(int d)
{
	u_int64_t	max_total_time = 0;
	int		cpu_of_max = 0;
	int		cpu;
	int             i;

	for (cpu = 0; cpu < ncpus; cpu++) {
		u_int64_t total_time = 0;

		for (i = 0; i < CPUSTATES; i++) {
			cp_diff[cpu][i] = cp_time[cpu][i] - cp_old[cpu][i];
			if (i != CP_IDLE)
				total_time += cp_diff[cpu][i];
		}
		if (total_time > max_total_time) {
			max_total_time = total_time;
			cpu_of_max = cpu;
		}
	}

	max_total_time += cp_diff[cpu_of_max][CP_IDLE];

	/* we've probably been interrupted by a signal... */
	if (max_total_time < 1) return -1; 

	return (100 - ((cp_diff[cpu_of_max][CP_IDLE] +
			(cp_diff[cpu_of_max][CP_NICE] * nicemod)) * 100) /
				max_total_time);
}
#endif

/* sets the cpu frequency */
void
set_freq(int d)
{
	int freq = domain[d].freqtab[domain[d].curfreq];

	if ((!daemonize) && (verbose))
		printf("%i MHz\n", freq);
	if (sysctlbyname(domain[d].setctl, NULL, NULL, &freq, sizeof(freq)) < 0) {
		fprintf(stderr, "estd: Cannot set CPU frequency (maybe you aren't root?)\n");
		exit(1);
	}
}

void
set_clockmod(int level)
{
#if !defined(__DragonFly__)
	if (!use_clockmod || level == -1)
		return;

	if ((!daemonize) && (verbose))
		printf("clockmod level: %i\n", level);
	if (sysctlbyname("machdep.clockmod.target", NULL, NULL, &level,
	    sizeof(level)) < 0) {
		fprintf(stderr, "estd: Cannot set clockmod level (maybe you aren't root?)\n");
		exit(1);
	}
#endif
}

/* need this callback for sorting the frequency-list */
int
freqcmp(const void *x, const void *y)
{
	return *((int *) x) - *((int *) y);
}

/* clean up the pidfile and clockmod on exit */
void
sighandler(int sig)
{
	set_clockmod(clockmod_max);
	exit(0);
}

/* switch strategy on SIGUSR{1,2} */
void
sigusrhandler(int sig)
{
	switch (sig) {
		case SIGUSR1:
				if (strategy>BATTERY) strategy--;
				break;
		case SIGUSR2:
				if (strategy<AGGRESSIVE) strategy++;
				break;
	}
}

int
main(int argc, char *argv[])
{
	int             ch;
	int             i;
	char            frequencies[SYSCTLBUF];	/* XXX Ugly */
	char           *fp;
	size_t          freqsize = SYSCTLBUF;
	int	            curstrat = strategy;
	int             d;
	FILE           *fexists;
#if defined(__DragonFly__)
	struct pidfh   *pdf;
#endif

	/* get command-line options */
	while ((ch = getopt(argc, argv, "vfdonACEGILPasbp:h:l:g:m:M:")) != -1)
		switch (ch) {
		case 'v':
			version();
			/* NOTREACHED */
		case 'f':
			listfreq = 1;
			break;
		case 'd':
			daemonize = 1;
			break;
		case 'o':
			verbose = 1;
			break;
		case 'n':
			nicemod = 1;
			break;
		case 'A':
			tech = TECH_ACPI;
			break;
		case 'C':
			#if !defined(__DragonFly__)
			 use_clockmod = 1;
			 break;
			#else
			 fprintf(stderr, "-C not available under DragonFly\n");
			 exit(1);
			#endif
		case 'E':
			tech = TECH_EST;
			break;
		case 'G':
			tech = TECH_GENERIC;
			break;
		case 'I':
			tech = TECH_INTREPID;
			break;
                case 'L':
                        tech = TECH_LOONGSON;
                        break;
		case 'P':
			tech = TECH_POWERNOW;
			break;
		case 'R':
			tech = TECH_ROCKCHIP;
			break;
		case 'a':
			strategy = AGGRESSIVE;
			break;
		case 's':
			strategy = SMOOTH;
			break;
		case 'b':
			strategy = BATTERY;
			break;
		case 'p':
			poll = atoi(optarg);
			break;
		case 'h':
			high = atoi(optarg);
			break;
		case 'l':
			low = atoi(optarg);
			break;
		case 'g':
			lowgrace = atoi(optarg);
			break;
		case 'm':
			minmhz = atoi(optarg);
			break;
		case 'M':
			maxmhz = atoi(optarg);
			break;
		default:
			usage();
			/* NOTREACHED */
		}

	ndomains = 1;
	domain = ecalloc(ndomains, sizeof(struct domain));

#if defined(__NetBSD__) || defined(__DragonFly__)
# if defined(__DragonFly__)
	if (kinfo_get_cpus(&ncpus)) {
		fprintf(stderr, "estd: Cannot get number of cpus\n");
		exit(1);
	}
	cp_time = ecalloc(ncpus, sizeof(struct kinfo_cputime));
	cp_old  = ecalloc(ncpus, sizeof(struct kinfo_cputime));
	cp_time_len = ncpus * sizeof(struct kinfo_cputime);
# elif defined(__NetBSD__)
	size_t ncpus_len = sizeof(ncpus);
	if (sysctlbyname("hw.ncpu", &ncpus, &ncpus_len, NULL, 0) != 0) {
		fprintf(stderr, "estd: Cannot get number of cpus\n");
		exit(1);
	}
# endif
	domain[0].ncpus = ncpus;
	domain[0].cpus = ecalloc(ncpus, sizeof(int));
	for (i = 0; i < domain[0].ncpus; i++)
		domain[0].cpus[i] = i;
#endif

	/* try to guess cpu-scaling technology */
	if (tech == TECH_UNKNOWN) {
		for (tech = 1; tech <= TECH_MAX; tech++) {
			if (sysctlbyname(freqctl[tech], &frequencies, &freqsize, NULL, 0) >= 0) break;
		}
		if (tech > TECH_MAX) {
			fprintf(stderr, "estd: Cannot guess CPU-scaling technology. (maybe you are missing some kernel-option?)\n");
			exit(1);
		}
	}

	if (tech == TECH_ACPI) {
		if (acpi_init()) {
			fprintf(stderr, "estd: Cannot ACPI P-States\n");
			exit(1);
		}
	} else {
		domain[0].freqctl = freqctl[tech];
		domain[0].setctl = setctl[tech];
	}

	if ((high <= low) || (low < 0) || (low > 100) || (high < 0) || (high > 100)) {
		fprintf(stderr, "estd: Invalid high/low watermark combination\n");
		exit(1);
	}

	if (poll < MIN_POLL) {
		fprintf(stderr, "estd: Poll interval is too low (minimum %i)\n", MIN_POLL);
		exit(1);
	}

	if (minmhz > maxmhz) {
		fprintf(stderr, "estd: Invalid minimum/maximum MHz combination\n");
		exit(1);
	}

	/* for each cpu domain... */
	for (d = 0; d < ndomains; d++) {
		/* get supported frequencies... */
		if (sysctlbyname(domain[d].freqctl, &frequencies, &freqsize, NULL, 0) < 0) {
			fprintf(stderr, "estd: Cannot get supported frequencies (maybe you forced the wrong CPU-scaling technology?)\n");
			exit(1);
		}
		fp = &frequencies[0];
		while ((domain[d].nfreqs < MAX_FREQS) && ((domain[d].freqtab[domain[d].nfreqs++] = strtol(fp, &fp, 10)) != 0));
		domain[d].nfreqs--;
		if (domain[d].nfreqs <= 0) {
			fprintf(stderr, "estd: No supported frequencies found?! (please report this error)\n");
			exit(1);
		}
		/* ...and sort them in ascending order */
		qsort(&domain[d].freqtab, domain[d].nfreqs, sizeof(domain[d].freqtab[0]), &freqcmp);
		/* some sanity checks */
		while ((domain[d].minidx < domain[d].nfreqs) && (domain[d].freqtab[domain[d].minidx] < minmhz))
			domain[d].minidx++;
		if (domain[d].minidx >= domain[d].nfreqs) {
			fprintf(stderr, "estd: Minimum Frequency is too high\n");
			exit(1);
		}
		domain[d].maxidx = domain[d].nfreqs - 1;
		while ((domain[d].maxidx > -1) && (domain[d].freqtab[domain[d].maxidx] > maxmhz))
		domain[d].maxidx--;
		if (domain[d].maxidx < 0) {
			fprintf(stderr, "estd: Maximum Frequency is too low\n");
			exit(1);
		}
		if (domain[d].freqtab[domain[d].minidx] > domain[d].freqtab[domain[d].maxidx]) {
			fprintf(stderr, "estd: No supported frequency within given range found\n");
			exit(1);
		}
	}

	if (listfreq) {
		printf("Supported frequencies (%s Mode):\n",techdesc[tech]);
		for (d = 0; d < ndomains; d++) {
			printf("Domain %d:\n", d);
			for (i = 0; i < domain[d].nfreqs; i++) {
				printf("%i MHz\n", domain[d].freqtab[i]);
			}
		}
		exit(0);
	}

	#if !defined(__DragonFly__)
	{
		char   *lastfp = NULL;
		char	clockmods[SYSCTLBUF];
		size_t	len = sizeof(clockmods);

		if (sysctlbyname("machdep.clockmod.available", &clockmods, &len,
		    NULL, 0) >= 0) {
			fp = &clockmods[0];
			while (fp != lastfp) {
				lastfp = fp;
				i = strtol(fp, &fp, 10);
				if (i < clockmod_min || clockmod_min == -1)
					clockmod_min = i;
				if (i > clockmod_max || clockmod_max == -1)
					clockmod_max = i;
			}
		}
	}
	#endif

	if ((fexists = fopen("/var/run/estd.pid", "r")) != NULL) {
		fprintf(stderr, "estd: Pidfile /var/run/estd.pid exists, remove it if you are sure it shouldn't be there (maybe another instance of estd is already running?)\n");
		fclose(fexists);
		exit(1);
	}
	
	/* all ok, here we go */
	if (daemonize) {
		if (fork()) {
			printf("estd: Forked\n");
			exit(0);
		}
	} else {
		printf("estd: Not detaching from terminal\n");
	}

#if defined(__DragonFly__)
	pdf = pidfile_open(NULL, 600, NULL);
	if (pdf == NULL) {
		fprintf(stderr, "estd: Cannot write pidfile (maybe you aren't root?)\n");
		exit(1);
	} else {
		pidfile_write(pdf);
	}
#else
	if (pidfile(NULL)) {
		fprintf(stderr, "estd: Cannot write pidfile (maybe you aren't root?)\n");
		exit(1);
	}
#endif
	
	/* init some vars and set inital frequency */
	signal(SIGHUP, SIG_IGN);
	signal(SIGINT, &sighandler);
	signal(SIGPIPE, &sighandler);
	signal(SIGTERM, &sighandler);
	signal(SIGUSR1, &sigusrhandler);
	signal(SIGUSR2, &sigusrhandler);

	for (d = 0; d < ndomains; d++) {
		domain[d].curfreq = domain[d].minidx;
		set_freq(d);
	}
	set_clockmod(clockmod_min);

	/* the big processing loop, we will only exit via signal */
	while (1) {
		get_cputime();
		for (d = 0; d < ndomains; d++) {
			domain[d].curcpu = get_cpuusage(d);
			if ((!daemonize) && (verbose))
				printf("estd: load(%d) %d\n", d, domain[d].curcpu);
			if (domain[d].curcpu != -1) {
				/* strategy can change anytime (SIGUSR) */ 
				curstrat = strategy;
				if ((domain[d].curfreq > domain[d].minidx) && (domain[d].curcpu < low)) {
					if (domain[d].lowtime < lowgrace)
						domain[d].lowtime += poll;

					if (domain[d].lowtime >= lowgrace) {
						if (curstrat == BATTERY)
							domain[d].curfreq = domain[d].minidx;
						else
							domain[d].curfreq--;
						set_freq(d);
						if (domain[d].curfreq == domain[d].minidx)
							set_clockmod(clockmod_min);
					}
				} else {
					domain[d].lowtime = 0;

					if ((domain[d].curfreq < domain[d].maxidx) && (domain[d].curcpu > high)) {
						if (curstrat == AGGRESSIVE)
							domain[d].curfreq = domain[d].maxidx;
						else
							domain[d].curfreq++;
						set_freq(d);
						if (domain[d].curfreq != domain[d].minidx)
							set_clockmod(clockmod_max);
					}
				}
			}
		}
		usleep(poll);
	}

	return 0;
}
