#include <core/random.h>

#include <stdio.h>
#include <assert.h>

#include "unit.h"

static void
test_random_in_range_one(int64_t min, int64_t max)
{
	int64_t result = pseudo_random_in_range(min, max);
	assert(min <= result && result <= max);
	if (min == max)
		printf("pseudo_random_in_range(%lld, %lld) = %lld\n",
		       (long long)min, (long long)max, (long long)result);

	result = real_random_in_range(min, max);
	assert(min <= result && result <= max);
	if (min == max)
		printf("real_random_in_range(%lld, %lld) = %lld\n",
		      (long long)min, (long long)max, (long long)result);
}

static void
test_random_in_range(void)
{
	header();

	test_random_in_range_one(INT64_MIN, INT64_MAX);
	test_random_in_range_one(INT64_MIN, INT64_MIN);
	test_random_in_range_one(INT64_MAX, INT64_MAX);
	test_random_in_range_one(-1, -1);
	test_random_in_range_one(0, 0);
	test_random_in_range_one(1, 1);

	test_random_in_range_one(INT64_MIN + 1, INT64_MAX - 1);
	test_random_in_range_one(INT64_MIN / 2, INT64_MAX / 2);
	test_random_in_range_one(INT64_MIN, INT64_MIN / 2);
	test_random_in_range_one(INT64_MAX / 2, INT64_MAX);
	test_random_in_range_one(-2, -1);
	test_random_in_range_one(1, 2);
	test_random_in_range_one(-1, 1);
	test_random_in_range_one(0, 1);

	footer();
}

int
main(void)
{
	random_init();

	test_random_in_range();

	random_free();
}
