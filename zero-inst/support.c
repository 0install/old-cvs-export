#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>

#include "support.h"

void *my_malloc(size_t size)
{
	void *new;

	new = malloc(size);
	if (!new) {
		fprintf(stderr, "zero-install: Out of memory");
		return NULL;
	}

	return new;
}

void *my_realloc(void *old, size_t size)
{
	void *new;

	new = realloc(old, size);
	if (!new) {
		fprintf(stderr, "zero-install: Out of memory");
		return NULL;
	}

	return new;
}

char *my_strdup(const char *str)
{
	int l = strlen(str) + 1;
	char *new;

	new = my_malloc(l);
	if (!new)
		return NULL;
		
	memcpy(new, str, l);

	return new;
}

void set_blocking(int fd, int blocking)
{
	if (fcntl(fd, F_SETFL, blocking ? 0 : O_NONBLOCK))
		perror("fcntl() failed");
}
