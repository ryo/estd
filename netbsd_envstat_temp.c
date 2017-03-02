#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <regex.h>
#include <prop/proplib.h>

#ifndef PATH_ENVSTAT
#define PATH_ENVSTAT "/usr/sbin/envstat"
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
	int result;
};

static void
prop_check_callback(void *arg, const char *key, prop_object_t obj)
{
	struct keychecker *keychecker;
	regmatch_t re_pmatch[1];
	double celsius;
	uint64_t num;

	keychecker = (struct keychecker *)arg;

	re_pmatch[0].rm_so = 0;
	re_pmatch[0].rm_eo = strlen(key);
	if (regexec(&keychecker->re, key, 1, re_pmatch, REG_STARTEND) != 0)
		return;

	switch (prop_object_type(obj)) {
	case PROP_TYPE_BOOL:
#ifdef NETBSD_ENVSTAT_DEBUG
		fprintf(stderr, "%s=%s\n", key, prop_bool_true(obj) ? "true" : "false");
#endif
		break;
	case PROP_TYPE_NUMBER:
		num = prop_number_unsigned_integer_value(obj);
		num -= 273150000;
		celsius = num / 1000000;
		if (celsius >= keychecker->limit)
			keychecker->result = 1;
		else
			keychecker->result = 0;
		break;
	case PROP_TYPE_STRING:
#ifdef NETBSD_ENVSTAT_DEBUG
		{
			const char *p;
			p = prop_string_cstring_nocopy(obj);
			fprintf(stderr, "%s=\"%s\"\n", key, p);
		}
#endif
		break;
	case PROP_TYPE_DATA:
#ifdef NETBSD_ENVSTAT_DEBUG
		fprintf(stderr, "%s=<PROP_TYPE_DATA>\n", key);
#endif
		break;
	default:
#ifdef NETBSD_ENVSTAT_DEBUG
		fprintf(stderr, "%s: UNKNOWN PROP TYPE\n", key);
#endif
		break;
	}
}

static int
check_overheat(const char *device, double limit)
{
	prop_object_t propobj;
	struct keychecker keychecker;
	FILE *fh;
	int rc;
	char pattern[128];
	char *xml;

	/* exec "envstat -x" and parse */
	fh = popen(PATH_ENVSTAT " -x", "r");
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
	if (propobj == NULL) {
		fprintf(stderr, "estd: cannot read result of `envstat -x`\n");
		return -1;
	}

	memset(&keychecker, 0, sizeof(keychecker));
	snprintf(pattern, sizeof(pattern), "%s\\.cur-value", device);

	rc = regcomp(&keychecker.re, pattern, REG_EXTENDED|REG_ICASE);
	keychecker.limit = limit;

	prop_iterate(prop_check_callback, (void *)&keychecker, propobj);
	prop_object_release(propobj);

	regfree(&keychecker.re);

	return keychecker.result;
}

int
is_overheat(const char *device, double celsius, unsigned int cachetime)
{
	static struct timespec ts_last, ts_now;
	static int result;

	if (device == NULL)
		return 0;

	clock_gettime(CLOCK_MONOTONIC, &ts_now);
	if ((ts_now.tv_sec - ts_last.tv_sec) > cachetime) {
		result = check_overheat(device, celsius);
		ts_last = ts_now;
	}

	return result;
}

#if 0
int
main(int argc, char *argv[])
{
	int fire;

	for (;;) {
		fire = check_overheat("coretemp[0-9]+", 59.0);
		printf("overheat=%d\n", fire);
		fflush(stdout);

		sleep(1);
	}
}
#endif
