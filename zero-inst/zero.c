#include <sys/mount.h>
#include <sys/wait.h>
#include <string.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include "lazyfs.h"

static void do_mount(const char *cache_dir, const char *mount_dir)
{
	struct lazy_mount_data data;

	data.version = 1;
	data.fd = open(cache_dir, O_RDONLY);
	if (data.fd == -1)
	{
		printf("Can't open '%s'\n", cache_dir);
		exit(1);
	}

	printf("Mounting %s on %s...\n", cache_dir, mount_dir);

	if (mount(cache_dir, mount_dir, "lazyfs", 
			0xC0ED0000 | MS_RDONLY | MS_NOSUID | MS_NODEV, &data))
	{
		perror("mount");
		_exit(EXIT_FAILURE);
	}
}

#define ERR(m) write(2, m, sizeof(m))

static int coda = -1;

static void sig_int(int signum)
{
	if (coda == -1)
		_exit(1);
	ERR("Interrupt: closing CODA connection...\n");
	close(coda);
	coda = -1;
	return;
}

int main(int argc, char **argv)
{
	const char *mount_point = "/uri";

	do_mount("/var/cache/zero-inst/", mount_point);

	printf("Ready...\n");

	{
		struct sigaction act;
		act.sa_handler = sig_int;
		sigemptyset(&act.sa_mask);
		act.sa_flags = 0;
		sigaction(SIGINT, &act, NULL);
	}
	
	coda = 0;

	while (coda != -1)
	{
		printf("waiting\n");
		system("cat /uri/.lazyfs-helper");
		break;
	}
	
	if (coda != -1)
		close(coda);

	if (mount_point)
	{
		printf("Unmounting...\n");
		while (umount(mount_point))
		{
			perror("umount");
			sleep(1);
		}
	}
	
	return EXIT_SUCCESS;
}
