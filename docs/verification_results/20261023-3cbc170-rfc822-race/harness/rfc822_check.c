// usage: rfc822_check race            : 8 threads call rfc822_datetime_format concurrently (build with TSan)
//        rfc822_check equal <old-sym> : compare against the 3cbc170 implementation linked as old_rfc822_datetime_format
#include "rfc822-datetime.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define THREADS 8
#define CALLS 200000

const char* old_rfc822_datetime_format(time_t time, rfc822_datetime_t datetime);

static void* worker(void* arg)
{
	time_t base = (time_t)(intptr_t)arg;
	rfc822_datetime_t datetime;
	unsigned long sum = 0;
	for (int i = 0; i < CALLS; i++)
	{
		const char* text = rfc822_datetime_format(base + (time_t)i * 3607, datetime);
		sum += text ? (unsigned char)text[0] : 0;
	}
	return (void*)(uintptr_t)sum;
}

static int race(void)
{
	pthread_t threads[THREADS];
	for (int i = 0; i < THREADS; i++)
	{
		pthread_create(&threads[i], NULL, worker, (void*)(intptr_t)(1000000000 + i * 86400 * 97));
	}
	for (int i = 0; i < THREADS; i++)
	{
		pthread_join(threads[i], NULL);
	}
	printf("race threads=%d calls_per_thread=%d\n", THREADS, CALLS);
	return 0;
}

static int compare(time_t value, unsigned long* checked)
{
	rfc822_datetime_t a, b;
	const char* x = rfc822_datetime_format(value, a);
	const char* y = old_rfc822_datetime_format(value, b);
	++*checked;
	if ((x == NULL) != (y == NULL) || (x && strcmp(x, y) != 0))
	{
		printf("MISMATCH time=%lld new=%s old=%s\n", (long long)value, x ? x : "(null)", y ? y : "(null)");
		return 1;
	}
	return 0;
}

static int equal(void)
{
	static const long long edges[] = {
		0LL, 1LL, -1LL, 59LL, 60LL, 86399LL, 86400LL,
		951782400LL, 951868799LL, 951868800LL,
		2147483647LL, 2147483648LL, -2147483648LL,
		4107456000LL, 4107542399LL, 4107542400LL,
		253402300799LL, 1792742400LL,
	};
	unsigned long checked = 0;
	int failures = 0;
	for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); i++)
	{
		failures += compare((time_t)edges[i], &checked);
	}
	for (long long t = -2208988800LL; t <= 253402300799LL; t += 86413LL)
	{
		failures += compare((time_t)t, &checked);
	}
	time_t now = time(NULL);
	for (long long d = -400000; d <= 400000; d++)
	{
		failures += compare(now + (time_t)d, &checked);
	}
	rfc822_datetime_t sample;
	printf("equal checked=%lu failures=%d sample_now=\"%s\" sample_epoch=\"%s\"\n", checked, failures,
	       rfc822_datetime_format(now, sample), rfc822_datetime_format(0, (char[30]){0}));
	return failures != 0;
}

int main(int argc, char** argv)
{
	if (argc > 1 && strcmp(argv[1], "race") == 0)
	{
		return race();
	}
	if (argc > 1 && strcmp(argv[1], "equal") == 0)
	{
		return equal();
	}
	fprintf(stderr, "usage: %s race|equal\n", argv[0]);
	return 2;
}
