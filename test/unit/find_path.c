#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "unit.h"

extern const char *
find_path(const char *);

int
main(int argc, char *argv[])
{
	header();
	plan(1);
	fail_unless(open(find_path(argv[0]), O_RDONLY) >= 0);
	ok(1, "find_path");
	check_plan();
	footer();
}
