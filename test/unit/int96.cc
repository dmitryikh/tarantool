#include <bit/int96.h>

#include "unit.h"

static void
test()
{
	header();
	plan(15);

	const uint64_t a = 0xFFFFFFFFFFFFFFFFull / 2;
	int96_num num, num1, num2;
	int96_set_unsigned(&num, 0);
	int96_set_unsigned(&num1, a);
	int96_set_unsigned(&num2, a);
	int96_invert(&num2);
	is(int96_is_neg_int64(&num2), true, "int96_is_neg_int64()");
	is(int96_extract_neg_int64(&num2), int64_t(-a), "int96_extract_neg_int64()");
	is(int96_is_uint64(&num), true, "int96_is_uint64()");
	is(int96_extract_uint64(&num), 0, "int96_extract_uint64()");
	int96_add(&num, &num1);
	is(int96_is_uint64(&num), true, "int96_is_uint64()");
	is(int96_extract_uint64(&num), a, "int96_extract_uint64()");
	int96_add(&num, &num1);
	is(int96_is_uint64(&num), true, "int96_is_uint64()");
	is(int96_extract_uint64(&num), a * 2, "int96_extract_uint64()");
	for (int i = 1; i < 1000; i++) {
		for(int j = 0; j < i; j++) {
			int96_add(&num, &num1);
			fail_if(!(!int96_is_uint64(&num) && !int96_is_neg_int64(&num)));
		}
		for(int j = 0; j < i - 1; j++) {
			int96_add(&num, &num2);
			fail_if(!(!int96_is_uint64(&num) && !int96_is_neg_int64(&num)));
		}
		int96_add(&num, &num2);
		fail_if(!int96_is_uint64(&num));
		fail_if(int96_extract_uint64(&num) != a * 2);
	}
	ok(1, "int96_add() in loop");
	int96_add(&num, &num2);
	is(int96_is_uint64(&num), true, "int96_is_uint64()");
	is(int96_extract_uint64(&num), a, "int96_extract_uint64()");
	int96_add(&num, &num2);
	is(int96_is_uint64(&num), true, "int96_is_uint64()");
	is(int96_extract_uint64(&num), 0, "int96_extract_uint64()");
	int96_add(&num, &num2);
	is(int96_is_neg_int64(&num), true, "int96_is_neg_int64()");
	is(int96_extract_neg_int64(&num), int64_t(-a), "int96_extract_neg_int64()");
	for (int i = 1; i < 1000; i++) {
		for(int j = 0; j < i; j++) {
			int96_add(&num, &num2);
			fail_if(!(!int96_is_uint64(&num) && !int96_is_neg_int64(&num)));
		}
		for(int j = 0; j < i - 1; j++) {
			int96_add(&num, &num1);
			fail_if(!(!int96_is_uint64(&num) && !int96_is_neg_int64(&num)));
		}
		int96_add(&num, &num1);
		fail_if(!(int96_is_neg_int64(&num)));
		fail_if(int96_extract_neg_int64(&num) != int64_t(-a));
	}

	check_plan();
	footer();
}

int
main(int, const char **)
{
	plan(1);
	test();
	check_plan();
}
