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

static int helper = -1;

static void sig_int(int signum)
{
	if (helper == -1)
		_exit(1);
	ERR("Interrupt: closing helper connection...\n");
	close(helper);
	helper = -1;
	return;
}

static void child_handle(int fd)
{
	struct stat info;
	int got;
	char buffer[1024] = "/usr";
	char *target = buffer + 5;

	got = read(fd, buffer + 4, sizeof(buffer) - 4);
	if (got <= 0)
	{
		perror("read");
		return;
	}
	printf("Create '%s'\n", buffer);

	if (lstat(buffer, &info))
	{
		perror("stat");
		return;
	}

	if (chdir("/var/cache/zero-inst"))
	{
		perror("chdir");
		return;
	}

	if (S_ISREG(info.st_mode))
	{
		if (link(buffer, target))
			perror("link");
	}
	else if (S_ISDIR(info.st_mode))
	{
		struct dirent *ent;
		FILE *ddd;
		DIR *dir;
		
		printf("Making directory\n");
		if (*target)
		{
			//sleep(1);
			if (mkdir(target, 0755))
				perror("mkdir");
			if (chdir(target))
			{
				perror("chdir");
				return;
			}
		}

		ddd = fopen("....", "wb");
		fprintf(ddd, "LazyFS\n");

		dir = opendir(buffer);
		if (!dir)
		{
			perror("opendir");
			return;
		}

		while ((ent = readdir(dir)))
		{
			struct stat i;
			char full[4096];
			
			if (*ent->d_name == '.')
			{
				if (ent->d_name[1] == '.' && !ent->d_name[2])
					continue;
				if (!ent->d_name[2])
					continue;
			}

			strcpy(full, buffer);
			strcat(full, "/");
			strcat(full, ent->d_name);
			if (lstat(full, &i))
			{
				perror("lstat");
				printf("[ %s ]\n", full);
				return;
			}
			
			fprintf(ddd, "%c %ld %ld %s",
				S_ISDIR(i.st_mode) ? 'd' :
				S_ISLNK(i.st_mode) ? 'l' :
				i.st_mode & 0111 ? 'x' : 'f',
				(long) i.st_size,
				(long) i.st_mtime,
				ent->d_name);

			fputc('\0', ddd);
		}
		
		fclose(ddd);
		if (rename("....", "..."))
			perror("rename");
	}
}

static void handle_fd(int fd)
{
	printf("Handle request on FD %d\n", fd);
	
	switch (fork())
	{
		case -1:
			perror("fork");
			return;
		case 0:
			child_handle(fd);
			_exit(0);
		default:
			return;
	}
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

		act.sa_handler = SIG_IGN;
		sigemptyset(&act.sa_mask);
		act.sa_flags = 0;
		sigaction(SIGCHLD, &act, NULL);
	}
	
	helper = open("/uri/.lazyfs-helper", O_RDONLY);

	while (helper != -1)
	{
		int got;
		int fd;
		char number[30];
		printf("waiting\n");

		got = read(helper, &number, sizeof(number));
		if (got <= 0)
		{
			if (got < 0)
				perror("read");
			break;
		}

		fd = atoi(number);
		handle_fd(fd);
		close(fd);
	}
	
	if (helper != -1)
		close(helper);

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
