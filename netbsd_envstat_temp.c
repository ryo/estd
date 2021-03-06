#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <regex.h>
#include <paths.h>
#include <prop/proplib.h>
#include <sys/envsys.h>

#ifndef CMD_ENVSTAT
#define CMD_ENVSTAT "/usr/sbin/envstat -c /etc/envsys.conf"
#endif
#ifndef _PATH_SYSMON
#define _PATH_SYSMON "/dev/sysmon"
#endif

static void
prop_iterate0(void (*cbfunc)(void *, const char *, prop_object_t), char *cbarg, char *key, size_t keylen, prop_object_t parent, prop_object_t obj)
{
	prop_object_iterator_t itr;
	prop_object_t obj2;
	const char *p;
	char *k;

	switch (prop_object_type(obj)) {
	case PROP_TYPE_BOOL:
	case PROP_TYPE_NUMBER:
	case PROP_TYPE_STRING:
	case PROP_TYPE_DATA:
		(*cbfunc)(cbarg, key, obj);
		break;

	case PROP_TYPE_ARRAY:
		itr = prop_array_iterator(obj);
		for (obj2 = prop_object_iterator_next(itr); obj2 != NULL;
		    obj2 = prop_object_iterator_next(itr)) {
			prop_iterate0(cbfunc, cbarg, key, keylen, obj, obj2);
		}
		prop_object_iterator_release(itr);
		break;
	case PROP_TYPE_DICTIONARY:
		itr = prop_dictionary_iterator(obj);
		for (obj2 = prop_object_iterator_next(itr); obj2 != NULL;
		    obj2 = prop_object_iterator_next(itr)) {
			prop_iterate0(cbfunc, cbarg, key, keylen, obj, obj2);
		}
		prop_object_iterator_release(itr);
		break;
	case PROP_TYPE_DICT_KEYSYM:
		p = prop_dictionary_keysym_cstring_nocopy(obj);
		k = key + strlen(key);
		if (key[0] != '\0')
			strlcat(key, ".", keylen);
		strlcat(key, p, keylen);
		obj2 = prop_dictionary_get_keysym(parent, obj);
		prop_iterate0(cbfunc, cbarg, key, keylen, obj, obj2);
		*k = '\0';
		break;
	default:
		fprintf(stderr, "estd: UNKNOWN PROP TYPE\n");
		break;
	}
}

static void
prop_iterate(void (*cbfunc)(void *, const char *, prop_object_t), char *cbarg, prop_object_t obj)
{
	char *key;
	size_t keylen = 1024;
	key = alloca(keylen);
	key[0] = '\0';
	prop_iterate0(cbfunc, cbarg, key, keylen, NULL, obj);
}


char *
freadin(FILE *fh)
{
#define READIN_BLOCKSIZE	(1024 * 8)
	char *buf, *p;
	size_t size, done, rc;

	size = READIN_BLOCKSIZE;
	buf = p = malloc(size);
	if (buf == NULL)
		return NULL;
	done = 0;
	for (;;) {
		rc = fread(p, 1, READIN_BLOCKSIZE, fh);
		if (rc < 0) {
			fprintf(stderr, "estd: fread: %s\n", strerror(errno));
			free(buf);
			return NULL;
		}
		done += rc;
		if (rc < READIN_BLOCKSIZE)
			break;

		size += READIN_BLOCKSIZE;
		p = realloc(buf, size);
		if (p == NULL) {
			fprintf(stderr, "estd: malloc: %s\n", strerror(errno));
			free(buf);
			return NULL;
		}
		buf = p;
		p = buf + done;
	}

	if (done == size) {
		p = realloc(buf, size + 1);
		if (p == NULL) {
			fprintf(stderr, "estd: malloc: %s\n", strerror(errno));
			free(buf);
			return NULL;
		}
		buf = p;
	}
	p = buf + done;
	*p = '\0';

	return buf;
}

struct keychecker {
	regex_t re;
	double limit;
	double max;
	int result;
};

static void
prop_check_callback(void *arg, const char *key, prop_object_t obj)
{
	struct keychecker *keychecker;
	regmatch_t re_pmatch[1];
	double degrees;
	uint64_t num;

	keychecker = (struct keychecker *)arg;

	re_pmatch[0].rm_so = 0;
	re_pmatch[0].rm_eo = strlen(key);
	if (regexec(&keychecker->re, key, 1, re_pmatch, REG_STARTEND) != 0)
		return;

	switch (prop_object_type(obj)) {
	case PROP_TYPE_BOOL:
#ifdef TEST
		fprintf(stderr, "%s=%s\n", key, prop_bool_true(obj) ? "true" : "false");
#endif
		break;
	case PROP_TYPE_NUMBER:
		num = prop_number_unsigned_integer_value(obj);
		num -= 273150000;
		degrees = num / 1000000;
		if (degrees >= keychecker->limit)
			keychecker->result = 1;
		if (keychecker->max < degrees)
			keychecker->max = degrees;
		break;
	case PROP_TYPE_STRING:
#ifdef TEST
		{
			const char *p;
			p = prop_string_cstring_nocopy(obj);
			fprintf(stderr, "%s=\"%s\"\n", key, p);
		}
#endif
		break;
	case PROP_TYPE_DATA:
#ifdef TEST
		fprintf(stderr, "%s=<PROP_TYPE_DATA>\n", key);
#endif
		break;
	default:
#ifdef TEST
		fprintf(stderr, "%s: UNKNOWN PROP TYPE\n", key);
#endif
		break;
	}
}

static int
check_overheat(const char *device, double limit, double *degrees_ret)
{
	prop_object_t propobj;
	struct keychecker keychecker;
	FILE *fh;
	int fd, rc, use_envstat = 0;
	char pattern[128];
	char *xml;

	/*
	 * <device> is able to specified as regexp.
	 *
	 * if <device> has prefix "envstat:",
	 * exec "/usr/sbin/envstat -c /etc/envstat.conf -x" and read from it.
	 * otherwise, read from /dev/sysmon directly.
	 *
	 * e.g.
	 *    "envstat:MyTempSensor0" - exec "envstat -c /etc/envstat.conf -x" and read from it
	 *    "envstat:coretemp0"     - ditto
	 *    "coretemp0"             - read coretemp0 from /dev/sysmon
	 *    "coretemp[0-9]+"        - read coretemp0,1,2,... from /dev/sysmon
	 */
	if (strncmp(device, "envstat:", 8) == 0) {
		use_envstat = 1;
		device += sizeof("envstat:") - 1;
	}

	if (use_envstat) {
		/* exec "envstat -x" and parse */
		fh = popen(CMD_ENVSTAT " -x", "r");
		if (fh == NULL) {
			fprintf(stderr, "popen: envstat: %s\n", strerror(errno));
			return -1;
		}
		xml = freadin(fh);
		pclose(fh);
		if (xml == NULL)
			return -1;
		propobj = prop_dictionary_internalize(xml);
		free(xml);
	} else {
		fd = open(_PATH_SYSMON, O_RDONLY);
		rc = prop_dictionary_recv_ioctl(fd,
		    ENVSYS_GETDICTIONARY, (prop_dictionary_t *)&propobj);
		close(fd);
		if (rc) {
			fprintf(stderr, "estd: cannot read %s: %s\n",
			    _PATH_SYSMON, strerror(errno));
			return -1;
		}
	}

	if (propobj == NULL) {
		fprintf(stderr, "estd: cannot read result of `envstat -x`\n");
		return -1;
	}

	memset(&keychecker, 0, sizeof(keychecker));
	snprintf(pattern, sizeof(pattern), "%s\\.cur-value", device);

	rc = regcomp(&keychecker.re, pattern, REG_EXTENDED|REG_ICASE);
	keychecker.limit = limit;
	keychecker.max = 0;

	prop_iterate(prop_check_callback, (void *)&keychecker, propobj);
	prop_object_release(propobj);

	regfree(&keychecker.re);

	if (degrees_ret != NULL)
		*degrees_ret = keychecker.max;

	return keychecker.result;
}

int
is_overheat(const char *device, double degrees, unsigned int cachetime, double *degrees_ret)
{
	static struct timespec ts_last, ts_now;
	static int result;

	if (device == NULL)
		return 0;

	clock_gettime(CLOCK_MONOTONIC, &ts_now);
	if ((ts_now.tv_sec - ts_last.tv_sec) > cachetime) {
		result = check_overheat(device, degrees, degrees_ret);
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
		fire = check_overheat("envstat:coretemp[0-9]+", 59.0);
		printf("overheat with envstat=%d\n", fire);
		fflush(stdout);

		fire = check_overheat("coretemp[0-9]+", 59.0);
		printf("overheat with sysmon=%d\n", fire);

		sleep(1);
	}
}
#endif
