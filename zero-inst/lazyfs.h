#ifndef __LINUX_LAZYFS_FS_H
#define __LINUX_LAZYFS_FS_H

#define LAZYFS_MAGIC 0x3069

struct lazy_mount_data
{
	int version;
	int fd;
};

#endif
