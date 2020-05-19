#include "unit.h"
#include "rope_common.h"

/******************************************************************/

static void
test_avl_rotations()
{
	header();
	plan(1);

	struct rope *rope = test_rope_new();

	/* counterclockwise single rotation. */
	test_rope_insert(rope, 0, "1");
	test_rope_insert(rope, 1, "2");
	test_rope_insert(rope, 2, "<");

	/* clockwise single rotation */
	test_rope_insert(rope, 0, "0");
	test_rope_insert(rope, 0, ">");

	/* counterclockwise double rotation */
	test_rope_insert(rope, 1, "*");
	/* clocckwise double rotation */
	test_rope_insert(rope, 3, "p");
	test_rope_insert(rope, 3, "p");
	test_rope_insert(rope, 3, "p");
	test_rope_insert(rope, 3, "p");
	test_rope_insert(rope, 3, "*");

	rope_delete(rope);
	ok(1, "test avl rotations");
	check_plan();

	footer();
}

int
main()
{
	plan(1);
	test_avl_rotations();
	check_plan();
	return 0;
}
