#include <stdio.h>
#include <unistd.h>

#include "child.h"

void child_run_request(const char *path, const char *leaf)
{
	printf("Child process handing %s : %s\n", path, leaf);

	sleep(2);
}
