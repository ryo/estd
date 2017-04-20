#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <regex.h>
#include <paths.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/sysctl.h>
#include <sys/sensors.h>

//#define TEST

#define SYSCTL_BUFSIZ 8192

static int
check_overheat(const char *device, double limit)
{
	regex_t re;
	regmatch_t re_pmatch[1];
	struct sensordev sensordev;
	struct sensor sensor;
	size_t sensordev_len = sizeof(sensordev);
	size_t sensor_len = sizeof(sensor);
	double value;
	int mib[] = {CTL_HW, HW_SENSORS, 0, 0, 0};
	int dev, j, rc, overheated = 0;
	char sysctlname[SYSCTL_BUFSIZ];
	char pattern[128];

	rc = regcomp(&re, device, REG_EXTENDED|REG_ICASE);

	for (dev = 0;; dev++) {
		mib[2] = dev;
		if (sysctl(mib, 3, &sensordev, &sensordev_len, NULL, 0) < 0) {
			if (errno == ENXIO)
				continue;
			break;
		}

		mib[2] = dev;
		mib[3] = SENSOR_TEMP;
		for (j = 0; j < sensordev.maxnumt[SENSOR_TEMP]; j++) {
			mib[4] = j;
			if (sysctl(mib, 5, &sensor, &sensor_len, NULL, 0) < 0) {
				fprintf(stderr, "sysctl: hw.sensors.%s.temp%d: %s\n", sensordev.xname, j, strerror(errno));
				continue;
			}

			if (sensor.flags & SENSOR_FINVALID)
				continue;

			snprintf(sysctlname, sizeof(sysctlname) - 1,
			    "hw.sensors.%s.temp%d", sensordev.xname, j);

			re_pmatch[0].rm_so = 0;
			re_pmatch[0].rm_eo = strlen(sysctlname);
			if (regexec(&re, sysctlname, 1, re_pmatch, REG_STARTEND) != 0)
				continue;

			value = (sensor.value - 273150000) / 1000000.0;

#ifdef TEST
			printf("check: %s=%.2f > %.2f\n", sysctlname, value, limit);
#endif

			if (value >= limit) {
				overheated = 1;
				goto done;
			}
		}
	}
 done:
	regfree(&re);

	return overheated;
}

int
is_overheat(const char *device, double degrees, unsigned int cachetime)
{
	static struct timespec ts_last, ts_now;
	static int result;

	if (device == NULL)
		return 0;

	clock_gettime(CLOCK_MONOTONIC, &ts_now);
	if ((ts_now.tv_sec - ts_last.tv_sec) > cachetime) {
		result = check_overheat(device, degrees);
		ts_last = ts_now;
	}

	return result;
}

#ifdef TEST
int
main(int argc, char *argv[])
{
	int fire;

	for (;;) {
		fire = check_overheat("hw\\.sensors\\.(cpu|acpitz|itherm)[0-9]+\\.temp[0-9]+", 65.0);
		printf("overheat=%d\n", fire);
		fflush(stdout);

		sleep(1);
	}
}
#endif
