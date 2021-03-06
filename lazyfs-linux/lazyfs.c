/*
 * LazyFS file system, Linux implementation
 *
 * Copyright (C) 2003  Thomas Leonard
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 * Code and ideas copied from various other places in the kernel.
 */


/* See the 'Technical' file for details. */

#include <linux/autoconf.h>
#include <linux/version.h>

#define LAZYFS_DEBUG

#define LAZYFS_MAGIC 0x3069

#define LAZYFS_MAX_LISTING_SIZE (100*1024)

#define GET_HOST_MAY_BLOCK 0x1
#define GET_HOST_DONT_START 0x2 /* Never issue a new request */

#include "config.h"

#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 5, 70)
# define LINUX_2_6_SERIES
#endif

#ifdef LINUX_2_6_SERIES
	/* 2.6 kernel */
# define SBI_VOID(sb) ((sb)->s_fs_info)
# define TIME_T struct timespec
/* (... format only stores times to 1 second accuracy anyway) */
# define TIMES_EQUAL(a, b) ((a).tv_sec == (b).tv_sec)
#include <linux/dcache.h>
#include <linux/namei.h>
#include <linux/mount.h>
#include <linux/vfs.h>

#else
	/* 2.4 kernel */
# define SBI_VOID(sb) ((sb)->u.generic_sbp)
# ifdef CONFIG_MODVERSIONS
#  define MODVERSIONS
#  include <linux/modversions.h>
# endif
# include <linux/locks.h>
# define TIME_T time_t
# define TIMES_EQUAL(a, b) ((a) == (b))
#endif

#define SBI(sb) ((struct lazy_sb_info *) SBI_VOID(sb))

#include <linux/module.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/smp_lock.h>
#include <linux/file.h>
#include <linux/poll.h>
#include <linux/list.h>

#include <asm/uaccess.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 4, 20)
# include "compat20.h"
#endif

const char platform[] = LAZYFS_PLATFORM;

/* Everyone waiting for the helper to fetch a file waits on
 * this queue. They all get woken up when the fetcher completes any
 * request, but all the ones that still haven't got their file
 * (info->fetching == 1) go back to sleep.
 */
static DECLARE_WAIT_QUEUE_HEAD(lazy_wait);

/* The helper waits here. Whenever a process sets fetching to 1, it
 * wakes up the helper and then sleeps on the lazy_wait queue above.
 */
static DECLARE_WAIT_QUEUE_HEAD(helper_wait);

/* Extra information attached to the super block */
struct lazy_sb_info {
	/* The root of the cache, passed in as a mount option */
	struct dentry *host_dentry;
	struct vfsmount *host_mnt;

	struct dentry *helper_dentry;	/* The .lazyfs-helper dentry */
	struct dentry *cache_dentry;	/* The .lazyfs-cache dentry */

	/* When a helper connects, we set helper_mnt.
	 * This is protected by fetching_lock.
	 */
	struct vfsmount *helper_mnt;

	/* A list of lazy_de_info structures which are going to be
	 * sent to the helper. If the helper quits before these are
	 * delivered, they will all return EIO. This list is protected
	 * by fetching_lock.
	 */
	struct list_head to_helper;

	/* Requests which have been given to the helper, but no reply
	 * yet.
	 */
	struct list_head requests_in_progress;
};

/* Extra information attached to each dentry */
struct lazy_de_info {
	struct dentry *dentry;	/* dentry->d_fsdata->dentry == dentry */
	int may_delete;		/* Used during mark-and-sweep */

	/* If a directory is dynamic then all lookups are passed to the
	 * helper.
	 */
	int dynamic;

	/* When a directory is opened we find the corresponding directory
	 * in the cache, and store its dentry here. host_dentry make go
	 * NULL=>dentry or dentry=>dentry, but never dentry=>NULL.
	 */
	struct dentry *host_dentry;

	/* list_dentry is for the '...' file. We simply use this to detect
	 * when the file has been replaced. Protected by a spinlock
	 * in ensure_cached.
	 */
	struct dentry *list_dentry;

	/* This is a list of requests for this item being processed, one
	 * per requesting user. If a user requests a file and there is no
	 * request for that user, add one. See struct lazy_user_request.
	 * Protected by fetching_lock.
	 */
	struct list_head request_list;

#ifdef LAZYFS_DEBUG
	struct list_head all_dinfo;
#endif
};

/* Extra information attached to each open file */
struct lazy_file_info {
	struct file *host_file;	/* NULL if not yet ready */
	int n_mappings;		/* Number of times mmap increased inode's
				   mmap count */
};

#ifdef LAZYFS_DEBUG

enum {
	R_DDD_FILE,
	R_DENTRY_INFO,
	R_FILE_HOST_FILE,
	R_HELPER,
	R_HOST_DENTRY,
	R_INFO_HOST_DENTRY,
	R_INFO_LIST_DENTRY,
	R_LIST_DENTRY,
	R_LISTING,
	R_MAPPING,
	R_PARENT_HOST,
	R_REQUEST,
	R_ROOT_CACHE_DENTRY,
	R_ROOT_HOST_DENTRY,
	R_ROOT_HOST_MNT,
	R_SBI,
	R_TARGET,
	R_USER_REQUEST,

	N_RESOURCES
};
static const char *resource_names[] = {
	"ddd_file",
	"dentry_info",
	"file->host_file",
	"helper",
	"host_dentry",
	"info_host_dentry",
	"info_list_dentry",
	"list_dentry",
	"listing",
	"mapping",
	"parent_host",
	"request",
	"root cache_dentry",
	"root host_dentry",
	"root host_mnt",
	"sbi",
	"target",
	"user_request"
};

static atomic_t resources[N_RESOURCES];

static void init_resources(void)
{
	int i;
	if (sizeof(resource_names) / sizeof(*resource_names) != N_RESOURCES)
		printk("Wrong array size!!\n");
	for (i = 0; i < N_RESOURCES; i++)
		atomic_set(&resources[i], 0);
}

LIST_HEAD(all_dinfo);

#if 0
static void show_resources(void)
{
	int i;
	int any_non_zero = 0;
	struct list_head *pos;

	for (i = 0; i < N_RESOURCES; i++) {
		int j = atomic_read(&resources[i]);
		if (j) {
			printk("  %s: %d\n", resource_names[i], j);
			any_non_zero = 1;
		}
	}
	if (!any_non_zero)
		printk("lazyfs: No resources still in use.\n");

	list_for_each(pos, &all_dinfo) {
		struct lazy_de_info *info = 
			list_entry(pos, struct lazy_de_info, all_dinfo);

		printk("Dentry still allocated: '%s' (%s) at %p\n",
				info->dentry->d_name.name,
				info->dentry->d_inode ? "real" : "negative",
				info->dentry);
	}
}
#endif

static inline void inc(int type)
{
	//printk("+ %s\n", m);
	atomic_inc(&resources[type]);
}
static inline void dec(int type)
{
	//printk("- %s\n", m);
	atomic_dec(&resources[type]);
}
static void show_refs(struct dentry *dentry, int indent)
{
	struct list_head *next;
	int i;

	for (i = 0; i < indent; i++)
		printk(" ");
	printk("'%s' [%d] %s %s at %p\n", dentry->d_name.name,
			atomic_read(&dentry->d_count),
			indent != 0 && d_unhashed(dentry) ? "(unhashed)" : "",
			!dentry->d_inode ? "(negative)" : "",
			dentry);

	next = dentry->d_subdirs.next;
	while (next != &dentry->d_subdirs) {
		struct dentry *c = list_entry(next, struct dentry, d_child);
		show_refs(c, indent + 2);
		next = next->next;
	}
}

#endif

/* Any change to the helper_list queues requires this lock */
static DECLARE_MUTEX(fetching_lock);

/* Setting finfo->host_file require this */
static spinlock_t host_file_lock = SPIN_LOCK_UNLOCKED;

/* Any change to an inode's u.generic_ip mapping counter needs this */
static spinlock_t mapping_lock = SPIN_LOCK_UNLOCKED;

/* Held when modifying the tree in any way. This is held for longer than
 * dcache_lock.
 */
static DECLARE_MUTEX(update_dir);

struct lazy_user_request {
	struct dentry *dentry;
	uid_t uid;
	struct list_head request_list;	/* Link in lazy_de_info->request_list */

	/* An lazy_user_request may be in one of two states:
	 * - in sbi->to_helper : about to be sent to helper.
	 * - in sbi->requests_in_progress: being handled by helper.
	 * Protected by fetching_lock.
	 */
	struct list_head helper_list; /* Link in one of lazy_sb_info's lists */
};

static struct super_operations lazyfs_ops;
static struct inode_operations lazyfs_dir_inode_operations;
static struct dentry_operations lazyfs_dentry_ops;
static struct file_operations lazyfs_file_operations;
static struct file_operations lazyfs_helper_operations;
static struct file_operations lazyfs_handle_operations;
static struct inode_operations lazyfs_link_operations;
static struct file_operations lazyfs_dir_file_operations;
static struct address_space_operations lazyfs_unmapped_aops;

static int ensure_cached(struct dentry *dentry);

/* Returns NULL if no such request is present.
 * Caller must hold fetching_lock.
 */
static struct lazy_user_request *
find_user_request(struct lazy_de_info *info, uid_t uid)
{
	struct list_head *head, *next;

	head = &info->request_list;
	next = head->next;

	while (next != head) {
		struct lazy_user_request *request = list_entry(next,
					struct lazy_user_request, request_list);

		if (request->uid == uid)
			return request;

		next = next->next;
	}

	return NULL;
}

/* Add a request for this user and dentry info.
 * There must not already be such a request.
 * Caller must hold fetching_lock.
 * 1 on success.
 */
static int
add_user_request(struct lazy_de_info *info, uid_t uid)
{
	struct dentry *dentry = info->dentry;
	struct super_block *sb = dentry->d_inode->i_sb;
	struct lazy_sb_info *sbi = SBI(sb);
	struct lazy_user_request *request;

	request = kmalloc(sizeof(struct lazy_user_request), GFP_KERNEL);

	if (!request)
		return 0;

	inc(R_USER_REQUEST);
	request->dentry = dget(dentry);
	request->uid = uid;
	INIT_LIST_HEAD(&request->request_list);
	INIT_LIST_HEAD(&request->helper_list);
	
	list_add_tail(&request->helper_list, &sbi->to_helper);
	list_add(&request->request_list, &info->request_list);

	return 1;
}

/* Requests can be destroyed when:
 * - On to_helper and helper closes connection.
 * - Error sending to helper (on pending list at this point).
 * - Helper closes request handle (normal case; on pending list).
 *
 * Therefore, a request cannot be destroyed by two independant events at once.
 *
 * Caller must hold fetching_lock.
 */
static void
destroy_request(struct lazy_user_request *request)
{
	if (!request)
		BUG();

	//printk("Removing request by user %d for '%s'\n",
	//		request->uid, request->dentry->d_name.name);

	if (list_empty(&request->request_list))
		BUG();
	if (list_empty(&request->helper_list))
		BUG();

	list_del(&request->helper_list);
	list_del(&request->request_list);
	dput(request->dentry);

	kfree(request);
	dec(R_USER_REQUEST);
}

/* Symlinks store their target in the inode. Free that here. */
static void
lazyfs_clear_inode(struct inode *inode)
{
	if (S_ISLNK(inode->i_mode) && inode->u.generic_ip) {
		if (inode->u.generic_ip != platform) {
			kfree(inode->u.generic_ip);
			dec(R_TARGET);
		}
	}

	if (inode->i_mapping != &inode->i_data) {
		printk("Warning: Freeding inode with mapping!\n");
	}
}

/* All information about an inode (size, mtime, etc) is stored in the
 * ... file of it's parent directory.
 */
static int
#ifdef LINUX_2_6_SERIES
lazyfs_dentry_revalidate(struct dentry *dentry, struct nameidata *nd)
#else
lazyfs_dentry_revalidate(struct dentry *dentry, int flags)
#endif
{
	if (!dentry->d_inode)
		return 1;	/* Negative dentries are always OK */

	if (dentry->d_parent == dentry) {
		return 1;
	} else {
		ensure_cached(dentry->d_parent);

		/* For directories, we also make sure the child list is
		 * up-to-date for following.
		 */
		if (S_ISDIR(dentry->d_inode->i_mode))
			ensure_cached(dentry);

		/* If entry is still hashed, OK, otherwise we must do
		 * the lookup again.
		 */
		return d_unhashed(dentry) == 0;
	}
	return 1;
}

static void
lazyfs_release_dentry(struct dentry *dentry)
{
	struct lazy_de_info *info = dentry->d_fsdata;

	if (dentry->d_inode)
		BUG();
	if (!info) {
		/* (could happen on OOM) */
		printk("Warning: lazyfs_release_dentry '%s' has no info\n",
				dentry->d_name.name);
		return;
	}
	if (!list_empty(&info->request_list))
		BUG();

#ifdef LAZYFS_DEBUG
	list_del(&info->all_dinfo);
#endif

	if (info->host_dentry) {
		dec(R_INFO_HOST_DENTRY);
		dput(info->host_dentry);
	}
	if (info->list_dentry) {
		dec(R_INFO_LIST_DENTRY);
		dput(info->list_dentry);
	}

	dentry->d_fsdata = NULL;
	kfree(info);
	dec(R_DENTRY_INFO);
}

/* Create a new inode with an unused inode number */
static inline struct inode *
lazyfs_new_inode(struct super_block *sb, mode_t mode,
		 loff_t size, TIME_T mtime, struct qstr *link_target)
{
	struct inode *inode;

	inode = new_inode(sb);
	if (!inode)
		return NULL;
	if (inode->i_sb != sb)
		BUG();

	inode->u.generic_ip = NULL;

	inode->i_mode = mode | 0644;	/* Always give read */
	inode->i_nlink = 1;
	inode->i_uid = 0;
	inode->i_gid = 0;
	inode->i_size = size;
	inode->i_atime = CURRENT_TIME;
	inode->i_ctime = inode->i_mtime = mtime;
	inode->i_data.a_ops = &lazyfs_unmapped_aops;
	if (S_ISDIR(mode)) {
		inode->i_op = &lazyfs_dir_inode_operations;
		inode->i_fop = &lazyfs_dir_file_operations;
		inode->i_mode |= 0111;
	} else if (S_ISREG(mode))
		inode->i_fop = &lazyfs_file_operations;
	else if (S_ISLNK(mode)) {
		char *target;

		if (link_target->len == strlen("@PLATFORM@") &&
		    strcmp(link_target->name, "@PLATFORM@") == 0) {
			target = (char *) platform;
		} else {
			target = kmalloc(link_target->len + 1, GFP_KERNEL);
			if (!target) {
				iput(inode);
				return NULL;
			}
			inc(R_TARGET);
			memcpy(target, link_target->name, link_target->len);
			target[link_target->len] = '\0';
		}
		inode->u.generic_ip = target;

		inode->i_op = &lazyfs_link_operations;
		inode->i_mode |= 0111;
	} else if (S_ISFIFO(mode)) {
		struct lazy_sb_info *sbi = SBI(sb);

		inode->i_fop = &lazyfs_helper_operations;
		inode->i_uid = sbi->host_dentry->d_inode->i_uid;
		inode->i_mode = S_IFIFO | 0400;
	} else
		BUG();
	return inode;
}

/* If parent_dentry is NULL, then this creates a new root dentry,
 * otherwise, refs the parent.
 * Only created during init and when opening a directory.
 * Must hold update_dir (except in init).
 */
static struct dentry *
new_dentry(struct super_block *sb, struct dentry *parent_dentry,
	   const char *leaf, mode_t mode, loff_t size, TIME_T mtime,
	   struct qstr *link_target)
{
	struct dentry *new = NULL;
	struct inode *inode = NULL;
	struct lazy_de_info *info;

	inode = lazyfs_new_inode(sb, mode, size, mtime, link_target);
	if (!inode)
		return NULL;

	if (parent_dentry) {
		struct qstr name;

		if (sb != parent_dentry->d_inode->i_sb)
			BUG();
		name.name = leaf;
		name.len = strlen(leaf);
		name.hash = full_name_hash(name.name, name.len);

		new = d_alloc(parent_dentry, &name);
	}
	else
		new = d_alloc_root(inode);

	if (!new)
		goto err;
	
	if (new->d_fsdata)
		BUG();

	info = kmalloc(sizeof(struct lazy_de_info), GFP_KERNEL);
	if (!info)
		goto err;
	inc(R_DENTRY_INFO);
	new->d_op = &lazyfs_dentry_ops;
	new->d_fsdata = info;
	info->dentry = new;
	info->host_dentry = NULL;
	info->list_dentry = NULL;
	info->may_delete = 0;
	info->dynamic = 0;
	INIT_LIST_HEAD(&info->request_list);

	INIT_LIST_HEAD(&info->all_dinfo);
	list_add(&info->all_dinfo, &all_dinfo);

	if (parent_dentry)
		d_add(new, inode);

	return new;
err:
	iput(inode);
	if (new)
		dput(new);
	return NULL;
}

/* Fill in the host_dentry and host_mnt fields from the mount options */
static inline int
host_from_mount_data(struct lazy_sb_info *sbi, const char *cache_dir)
{
	struct nameidata host_root;
	struct inode *inode;
	int err = 0;

	if (!cache_dir || !*cache_dir)
	{
		printk("lazyfs: No cache directory specified for mount!\n");
		return -ENOTDIR;
	}

#ifdef LINUX_2_6_SERIES
	if (path_lookup(cache_dir, LOOKUP_DIRECTORY | LOOKUP_FOLLOW,
				&host_root)) {
#else
	if (!path_init(cache_dir,
		LOOKUP_POSITIVE | LOOKUP_DIRECTORY | LOOKUP_FOLLOW,
		&host_root) || path_walk(cache_dir, &host_root)) {
#endif
		printk("lazyfs: Can't find cache directory '%s'\n", cache_dir);
		return -ENOTDIR;
	}

	inode = host_root.dentry->d_inode;
	if (!inode || !S_ISDIR(inode->i_mode)) {
		printk("lazyfs: Cache is not a valid directory\n");
		err = -ENOTDIR;
	} else {
		sbi->host_dentry = dget(host_root.dentry);
		inc(R_ROOT_HOST_DENTRY);
		sbi->host_mnt = mntget(host_root.mnt);
		inc(R_ROOT_HOST_MNT);
	}
	
	path_release(&host_root);

	return err;
}

static int
lazyfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct lazy_sb_info *sbi = NULL;
	struct qstr link_target;

	if (sb->s_count != S_BIAS) {
		printk("lazyfs_fill_super: wrong count "
			"(got %x, expecting %x)\n"
			"Probably, this module was compiled with different \n"
			"kernel headers or config, or a different compiler\n"
			"version. Refusing to start.\n",
			sb->s_count, S_BIAS);
		return -EINVAL;
	}

	sb->s_flags |= MS_RDONLY | MS_NODEV | MS_NOSUID;

	sbi = kmalloc(sizeof(struct lazy_sb_info), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;
	SBI_VOID(sb) = sbi;
	sbi->host_dentry = NULL;
	sbi->host_mnt = NULL;
	sbi->cache_dentry = NULL;
	inc(R_SBI);

	if (host_from_mount_data(sbi, (const char *) data))
		goto err;

	sb->s_blocksize = 1024;
	sb->s_blocksize_bits = 10;
	sb->s_magic = LAZYFS_MAGIC;
	sb->s_op = &lazyfs_ops;

	/* Create / */
	sb->s_root = new_dentry(sb, NULL, "/", S_IFDIR, 0,
			sbi->host_dentry->d_inode->i_mtime, NULL);
	if (!sb->s_root)
		goto err;

	/* Create /.lazyfs-cache */
	link_target.name = (char *) data;
	link_target.len = strlen(link_target.name);
	sbi->cache_dentry = new_dentry(sb, sb->s_root, ".lazyfs-cache",
			S_IFLNK, 0, CURRENT_TIME, &link_target);
	if (!sbi->cache_dentry)
		goto err;
	inc(R_ROOT_CACHE_DENTRY);

	/* Create /.lazyfs-helper */
	sbi->helper_dentry = new_dentry(sb, sb->s_root, ".lazyfs-helper",
			S_IFIFO, 0, CURRENT_TIME, NULL);
	if (!sbi->helper_dentry)
		goto err;

	sbi->helper_mnt = NULL;
	INIT_LIST_HEAD(&sbi->to_helper);
	INIT_LIST_HEAD(&sbi->requests_in_progress);

	return 0;
err:
	if (sbi->cache_dentry) {
		dput(sbi->cache_dentry);
		dec(R_ROOT_CACHE_DENTRY);
	}
	if (sbi->host_dentry) {
		dput(sbi->host_dentry);
		dec(R_ROOT_HOST_DENTRY);
	}
	if (sbi->host_mnt) {
		mntput(sbi->host_mnt);
		dec(R_ROOT_HOST_MNT);
	}
	if (sbi) {
		kfree(sbi);
		dec(R_SBI);
	}
	if (sb->s_root)
		dput(sb->s_root);
	return -EINVAL;
}

#ifdef LINUX_2_6_SERIES
struct super_block *lazyfs_get_sb(struct file_system_type *fs_type,
				 int flags, const char *dev_name, void *data)
{
	return get_sb_nodev(fs_type, flags, data, lazyfs_fill_super);
}
#else
static struct super_block *
lazyfs_read_super(struct super_block *sb, void *data, int silent)
{
	int err;

	err = lazyfs_fill_super(sb, data, silent);

	return err ? NULL : sb;
}
#endif
	
/* host_dentry is the directory in the cache which corresponds to
 * dentry. Make sure it contains a '...' file, and return that.
 * NULL if the file needs fetching or updating.
 */
static inline struct dentry *
get_host_dir_dentry(struct dentry *dentry, struct dentry *host_dentry)
{
	struct dentry *list_dentry;
	struct dentry *old;
	struct lazy_de_info *info = dentry->d_fsdata;
	struct qstr ddd_name;

	ddd_name.name = "...";
	ddd_name.len = 3;
	ddd_name.hash = full_name_hash(ddd_name.name, ddd_name.len);

	down(&host_dentry->d_inode->i_sem);
	list_dentry = lookup_hash(&ddd_name, host_dentry);
	up(&host_dentry->d_inode->i_sem);

	if (IS_ERR(list_dentry))
		return list_dentry;
	inc(R_LIST_DENTRY);
	if (!list_dentry->d_inode)
		goto fetch;
	if (!S_ISREG(list_dentry->d_inode->i_mode))
		goto fetch;

	/* Cache host_dentry */
	old = info->host_dentry;
	info->host_dentry = dget(host_dentry);
	inc(R_INFO_HOST_DENTRY);
	if (old) {
		dput(old);
		dec(R_INFO_HOST_DENTRY);
	}

	return list_dentry;
fetch:
	dec(R_LIST_DENTRY);
	dput(list_dentry);
	return NULL;
}

/* Try to open parent_host/dentry.name. If it works, and it has the correct
 * type, and (for directories) has a '...' file, return it (or the '...').
 *
 * Returns NULL if there is no error, but the host does not exist yet, or
 * if it exists, but is the wrong type of object.
 */
static struct dentry *try_get_host_dentry(struct dentry *dentry)
{
	struct dentry *host_dentry;
	mode_t mode, host_mode;
	struct qstr name = dentry->d_name;	/* Copy, for hash */
	struct super_block *sb = dentry->d_inode->i_sb;

	if (dentry != sb->s_root) {
		struct dentry *parent_dentry = dget(dentry->d_parent);
		struct lazy_de_info *parent_info;
		struct dentry *parent_host;

		if (parent_dentry == dentry) {
			/* File no longer exists (unlinked) */
			dput(parent_dentry);
			printk("lazyfs: try_get_host_dentry(%s): deleted\n",
					dentry->d_name.name);
			return ERR_PTR(-ENOENT);
		}

		/* parent_host can't be NULL, but it can change under us */
		parent_info = parent_dentry->d_fsdata;
		parent_host = dget(parent_info->host_dentry);
		dput(parent_dentry);
		if (!parent_host)
			BUG();
		inc(R_PARENT_HOST);

		down(&parent_host->d_inode->i_sem);
		host_dentry = lookup_hash(&name, parent_host);
		up(&parent_host->d_inode->i_sem);

		dput(parent_host);
		dec(R_PARENT_HOST);
	} else {
		/* Looking up the root */
		struct lazy_sb_info *sbi = SBI(sb);
		host_dentry = dget(sbi->host_dentry);
	}

	if (IS_ERR(host_dentry))
		return host_dentry;
	inc(R_HOST_DENTRY);

	if (!host_dentry)
		BUG();

	if (!host_dentry->d_inode)
		goto fetch;

	mode = dentry->d_inode->i_mode;
	host_mode = host_dentry->d_inode->i_mode;

	if (S_ISREG(mode)) {
		if (!S_ISREG(host_mode))
			goto fetch;
		if (host_dentry->d_inode->i_size != dentry->d_inode->i_size ||
		    !TIMES_EQUAL(host_dentry->d_inode->i_mtime,
			    	 dentry->d_inode->i_mtime)) {
			/* User space helper shouldn't really let this happen */
			printk("Cache out-of-date for '%s'... refetching\n",
					dentry->d_name.name);
			goto fetch;
		}
		return host_dentry;	/* File, OK */
	} else if (S_ISDIR(mode)) {
		struct dentry *list_dentry;

		if (!S_ISDIR(host_mode))
			goto fetch;

		list_dentry = get_host_dir_dentry(dentry, host_dentry);
		dput(host_dentry);
		dec(R_HOST_DENTRY);

		return list_dentry;
	}

	BUG();

fetch:
	/* host_dentry exists, but is negative or the wrong type */
	dput(host_dentry);
	dec(R_HOST_DENTRY);
	return NULL;
}

/* Return the host dentry for this /uri dentry. Its parent directory must
 * already have one. If the host inode does not yet exist, it sleeps until the
 * helper creates it (if blocking is 1).  If 'dentry' is a directory, then the
 * returned value will be the dentry of the '...' file. The host_dentry for the
 * directory itself is cached.
 */
static struct dentry *get_host_dentry(struct dentry *dentry, int flags)
{
	int blocking = (flags & GET_HOST_MAY_BLOCK) != 0;
	int dont_start = (flags & GET_HOST_DONT_START) != 0;
	struct super_block *sb = dentry->d_inode->i_sb;
	struct lazy_sb_info *sbi = SBI(sb);
	struct dentry *host_dentry;
	struct lazy_de_info *info = dentry->d_fsdata;
	int first_try = 1;
	DECLARE_WAITQUEUE(wait, current);

	if (!info)
		BUG();

	add_wait_queue(&lazy_wait, &wait);
	do {
		int start_fetching = 0;

		host_dentry = try_get_host_dentry(dentry);
		if (host_dentry)
			goto out;	/* Success or fatal error */
	
		/* Doesn't exist, or exists but is the wrong type, or
		 * missing '...' (for directories).
		 */

		host_dentry = ERR_PTR(-EIO);
		if (!first_try)
			goto out;
		first_try = 0;

		/* Start a fetch, if there isn't one already */
		down(&fetching_lock);

		if (sbi->helper_mnt) {
			if (find_user_request(info, current->uid)) {
				host_dentry = ERR_PTR(0);
			} else if (dont_start) {
				host_dentry = ERR_PTR(-EIO);
			} else if (add_user_request(info, current->uid)) {
				start_fetching = 1;
				host_dentry = ERR_PTR(0);
			} else
				host_dentry = ERR_PTR(-ENOMEM);
		} /* else -EIO */
		
		up(&fetching_lock);
		if (host_dentry)
			goto out;
		if (start_fetching)
			wake_up_interruptible(&helper_wait);
		/* else someone else is already fetching it */

		host_dentry = ERR_PTR(-EAGAIN);
		if (!blocking)
                        goto out;

		while (1) {
			current->state = TASK_INTERRUPTIBLE;
			down(&fetching_lock);
			if (!find_user_request(info, current->uid)) {
				up(&fetching_lock);
				break;	/* Fetch request completed */
			}
			up(&fetching_lock);

			if (signal_pending(current)) {
				host_dentry = ERR_PTR(-ERESTARTSYS);
				goto out;
			}
			
			schedule();
		}
	} while (1);

out:
        current->state = TASK_RUNNING;
        remove_wait_queue(&lazy_wait, &wait);

        return host_dentry;
}

#define isdigit(c) ((c) >= '0' && (c) <= '9')
__inline static int atoi(const char **s) 
{
int i = 0;
while (isdigit(**s))
  i = i*10 + *((*s)++) - '0';
return i;
}

/* Mark every child of this directory as a candidate for removal.
 * Called with 'update_dir' lock held.
 */
static inline void mark_children_may_delete(struct dentry *dentry)
{
	struct super_block *sb = dentry->d_inode->i_sb;
	struct lazy_sb_info *sbi = SBI(sb);
	struct list_head *head, *next;

	spin_lock(&dcache_lock);
	head = &dentry->d_subdirs;
	next = head->next;

	while (next != head) {
		struct dentry *child = list_entry(next, struct dentry, d_child);
		struct lazy_de_info *info = child->d_fsdata;
		next = next->next;

		if (d_unhashed(child) || !child->d_inode)
			continue;

		if (child != sbi->helper_dentry && child != sbi->cache_dentry)
			info->may_delete = 1;
	}

	spin_unlock(&dcache_lock);
}

/* Drop our reference to dentry. If noone else holds a reference, add it to
 * the to_be_removed list.
 * Called with dcache_lock held.
 */
static void genocide_one(struct dentry *dentry, struct list_head *to_be_removed)
{
	if (dentry->d_parent == dentry)
		BUG();

	/* Don't allow listing, etc of this directory */
	if (S_ISDIR(dentry->d_inode->i_mode))
		dentry->d_inode->i_flags |= S_DEAD;

	/* Unhash it (unhashing seems to be safe provided there are no
	 * child dentries).
	 * This is identical to d_drop, but without taking dcache_lock again.
	 */
#ifdef LINUX_2_6_SERIES
	__d_drop(dentry);
#else
	list_del(&dentry->d_hash);
	INIT_LIST_HEAD(&dentry->d_hash);
#endif

	/* Turn it into its own subtree */
	list_del_init(&dentry->d_child);
	if (atomic_read(&dentry->d_count) > 1) {
		//printk("Dentry '%s' will be freed later\n",
		//		dentry->d_name.name);
		/* Don't force it to stay */
		atomic_dec(&dentry->d_count);
	} else {
		/* (reusing d_child; noone else has a ref anyway) */
		list_add(&dentry->d_child, to_be_removed);
	}

	/* Break link to parent */
	atomic_dec(&dentry->d_parent->d_count);
	dentry->d_parent = dentry;
}

/*
 * This removes the whole subtree starting at 'root', trying to cope with
 * the fact that other people may be using some of the dentries.
 *
 * We scan through the whole subtree, turning each node into its own
 * subtree. If anyone else has a reference to it, we drop our reference and do
 * nothing more (the other user will free it later). If we hold the only
 * reference then we add the node to our to_be_removed list, and dput them all
 * at the end (for locking reasons).
 *
 * Called with update_dir held.
 */
static void my_d_genocide(struct dentry *root)
{
	struct dentry *this_parent = root, *parent;
	struct list_head *next, *tmp;
	LIST_HEAD(to_be_removed);

	spin_lock(&dcache_lock);
repeat:
	next = this_parent->d_subdirs.next;
resume:
	while (next != &this_parent->d_subdirs) {
		struct list_head *tmp = next;
		struct dentry *dentry = list_entry(tmp, struct dentry, d_child);
		next = tmp->next;
		if (d_unhashed(dentry) || !dentry->d_inode)
			continue;
		if (!list_empty(&dentry->d_subdirs)) {
			this_parent = dentry;
			goto repeat;
		}
		genocide_one(dentry, &to_be_removed);
	}

	/* Moving up to parent */

	next = this_parent->d_child.next; 
	parent = this_parent->d_parent;
	genocide_one(this_parent, &to_be_removed);
		
	if (this_parent != root) {
		this_parent = parent;
		goto resume;
	}
	spin_unlock(&dcache_lock);

	list_for_each_safe(next, tmp, &to_be_removed) {
		struct dentry *kid = list_entry(next, struct dentry, d_child);
		//printk("Removing '%s' now\n", kid->d_name.name);
		list_del_init(&kid->d_child);
		dput(kid);
	}
}

/* Called with update_dir help */
static void sweep_marked_children(struct dentry *dentry)
{
	struct list_head *head, *next;

restart:
	spin_lock(&dcache_lock);
	head = &dentry->d_subdirs;
	next = head->next;

	while (next != head) {
		struct dentry *child = list_entry(next, struct dentry, d_child);
		struct lazy_de_info *info = child->d_fsdata;
		next = next->next;

		if (d_unhashed(child) || !child->d_inode)
			continue;

		if (!info->may_delete)
			continue;

		spin_unlock(&dcache_lock);
		my_d_genocide(child);
		goto restart;
	}

	spin_unlock(&dcache_lock);
}

/* For directories, returns false but updates time */
static inline int
has_changed(struct inode *i, mode_t mode, size_t size, TIME_T time,
	    struct qstr *link_target)
{
	if ((i->i_mode & ~0777) != (mode & ~0777))
		return 1;

	if (S_ISDIR(mode)) {
		i->i_size = size;
		i->i_mtime = time;
		return 0;
	}

	if (i->i_size != size || !TIMES_EQUAL(i->i_mtime, time))
		return 1;

	if (i->i_mode != mode)
		return 1;

	if (S_ISLNK(mode)) {
		char *old = i->u.generic_ip;
		return strncmp(old, link_target->name, link_target->len) ||
			strlen(old) != link_target->len;
	}

	return 0;
}

/* The file list for a directory has changed.
 * Update it by parsing the new list of contents read from '...'.
 * Must be called with 'update_dir' lock held.
 */
static inline int
add_dentries_from_list(struct dentry *dir, const char *listing, int size)
{
	struct super_block *sb = dir->d_inode->i_sb;
	struct lazy_de_info *info = dir->d_fsdata;
	const char *end = listing + size;

	if (strncmp(listing, "LazyFS Dynamic\n", 15) == 0)
	{
		if (size != 15)
			goto bad_list;
		/* TODO: Wipe the old contents? */
		info->dynamic = 1;
		return 0;
	}
	info->dynamic = 0;

	/* Check for the magic string */
	if (size < 7 || strncmp(listing, "LazyFS\n", 7) != 0)
		goto bad_list;
	listing += 7;

	mark_children_may_delete(dir);

	while (listing < end) {
		struct dentry *existing;
		mode_t mode = 0644;
		struct qstr name;
		struct qstr link_target;
		off_t size;
		TIME_T time;

		switch (*(listing++)) {
			case 'f': mode |= S_IFREG; break;
			case 'x': mode |= S_IFREG | 0111; break;
			case 'd': mode |= S_IFDIR; break;
			case 'l': mode |= S_IFLNK; break;
			default: goto bad_list;
		}
		if (*(listing++) != ' ')
			goto bad_list;
		
		if (!isdigit(*listing))
			goto bad_list;
		size = atoi(&listing);
		if (*(listing++) != ' ')
			goto bad_list;

		if (!isdigit(*listing))
			goto bad_list;
#ifdef LINUX_2_6_SERIES
		time.tv_sec = atoi(&listing);
		time.tv_nsec = 0;
#else
		time = atoi(&listing);
#endif
		if (*(listing++) != ' ')
			goto bad_list;

		name.name = listing;
		name.len = strlen(name.name);
		listing += name.len + 1;
		if (!name.len)
			goto bad_list;

		if (listing == end + 1)
			goto bad_list;	/* Last line not terminated */

		if (S_ISLNK(mode)) {
			link_target.name = listing;
			link_target.len = strlen(link_target.name);
			listing += link_target.len + 1;
			if (!link_target.len)
				goto bad_list;
		}
		else
			link_target.len = 0;
		
		if (listing == end + 1)
			goto bad_list;	/* Last line not terminated */

		if (name.len == 1 && name.name[0] == '.')
			goto bad_list;	/* Can't have '.' */
		if (name.len == 2 && name.name[0] == '.' && name.name[1] == '.')
			goto bad_list;	/* Can't have '..' */
		if (strchr(name.name, '/'))
			goto bad_list;	/* Can't have '/' in a name */

		name.hash = full_name_hash(name.name, name.len);
		existing = d_lookup(dir, &name);
		if (existing && !existing->d_inode) {
			/* Unhash any negative dentry */
			d_drop(existing);
			dput(existing);
			existing = NULL;
		}

		if (existing) {
			struct inode *i = existing->d_inode;
			if (has_changed(i, mode, size, time, &link_target)) {
				my_d_genocide(existing);
				new_dentry(sb, dir, name.name,
					   mode, size, time, &link_target);
			} else {
				struct lazy_de_info *info = existing->d_fsdata;
				info->may_delete = 0;
			}
			dput(existing);
		} else {
			new_dentry(sb, dir, name.name, mode, size, time,
					&link_target);
		}
	}

	sweep_marked_children(dir);

	if (listing != end)
		BUG();

	return 0;

bad_list:
	printk("lazyfs: '...' file is invalid!\n");

	return -EIO;
}

/* Open this file and read it into a new kmalloc'd block. Return the
 * new block. The block is \0 terminated.
 */
static inline char *
open_and_read(struct dentry *dentry, struct vfsmount *mnt,
	      int max_size, off_t *size_out)
{
	off_t offset = 0;
	off_t size;
	char *listing;
	struct file *file;

	/* Open for reading (frees mnt and dentry) */
	file = dentry_open(dget(dentry), mntget(mnt), 0); /* 0 = RO */
	if (IS_ERR(file))
		return (char *) file;
	inc(R_DDD_FILE);

	size = file->f_dentry->d_inode->i_size;
	if (size < 0 || size > max_size) {
		printk("lazyfs: file too big\n");
		fput(file);
		dec(R_DDD_FILE);
		return ERR_PTR(-E2BIG);
	}

	listing = kmalloc(size + 1, GFP_KERNEL);
	if (!listing) {
		fput(file);
		dec(R_DDD_FILE);
		return ERR_PTR(-ENOMEM);
	}
	listing[size] = '\0';
	inc(R_LISTING);

	while (offset < size) {
		int got;

		got = kernel_read(file, offset, listing + offset,
				  size - offset);
		if (got <= 0) {
			printk("lazyfs: error reading file (%d)\n", got);
			fput(file);
			dec(R_DDD_FILE);
			kfree(listing);
			dec(R_LISTING);
			return ERR_PTR(got < 0 ? got : -EIO);
		}
		offset += got;
	}
	fput(file);
	dec(R_DDD_FILE);

	if (offset != size)
		BUG();

	*size_out = size;
	return listing;
}

/* Make sure the dcache reflects the contents of '...'. If '...' is
 * missing, try to fetch it now.
 */
static int ensure_cached(struct dentry *dentry)
{
	struct lazy_de_info *info = dentry->d_fsdata;
	struct dentry *list_dentry = NULL;
	int err;
	char *listing;
	off_t size;
	struct super_block *sb = dentry->d_inode->i_sb;
	struct lazy_sb_info *sbi = SBI(sb);

	if (!S_ISDIR(dentry->d_inode->i_mode))
		BUG();

	list_dentry = get_host_dentry(dentry, GET_HOST_MAY_BLOCK);

	down(&update_dir);

	if (IS_ERR(list_dentry)) {
		// Since ... was missing and couldn't be fetched, this
		// directory shouldn't exist. Remove it.
		//printk("Error from ensure_cached(%s): %ld\n", dentry->d_name.name, PTR_ERR(list_dentry));
		if (!d_unhashed(dentry)) {
			//printk("Removing...\n");
			my_d_genocide(dentry);
		}
		else if (!sbi->helper_mnt) {
			//printk("ensure_cached: Error, but no helper\n");
		} else {
			//printk("But not yet hashed\n");
		}
		up(&update_dir);
		return PTR_ERR(list_dentry);
	}

	if (list_dentry == info->list_dentry) {
		/* Already cached... do nothing */
		up(&update_dir);
		dput(list_dentry);
		dec(R_LIST_DENTRY);
		return 0;
	}

	/* Switch to the new version */
	if (info->list_dentry) {
		dput(info->list_dentry);
		dec(R_INFO_LIST_DENTRY);
	}
	info->list_dentry = dget(list_dentry);
	inc(R_INFO_LIST_DENTRY);

	listing = open_and_read(list_dentry, sbi->host_mnt,
				LAZYFS_MAX_LISTING_SIZE, &size);
	dput(list_dentry);
	dec(R_LIST_DENTRY);
	if (IS_ERR(listing)) {
		up(&update_dir);
		return PTR_ERR(listing);
	}

	err = add_dentries_from_list(dentry, listing, size);

	kfree(listing);
	dec(R_LISTING);

	if (err == -EIO) {
		struct inode *dir;
		list_dentry = dget(info->list_dentry);
		inc(R_LIST_DENTRY);
		dir = list_dentry->d_parent->d_inode;
		/* File is corrupted. Delete it. */
		down(&dir->i_sem);
		if (vfs_unlink(dir, list_dentry))
			printk("Error unlinking broken ... file!\n");
		up(&dir->i_sem);
		dput(list_dentry);
		dec(R_LIST_DENTRY);
	}

	up(&update_dir);

	return err;
}

static void
lazyfs_put_super(struct super_block *sb)
{
	struct lazy_sb_info *sbi = SBI(sb);

	/* We may have moved some dentries to the unused list.
	 * Need to free them here (Linux doesn't do it for us).
	 */
	shrink_dcache_sb(sb);

	if (sbi) {
		if (!list_empty(&sbi->to_helper))
			BUG();
		if (!list_empty(&sbi->requests_in_progress))
			BUG();
		if (sbi->helper_mnt)
			BUG();
		
		dput(sbi->cache_dentry);
		dec(R_ROOT_CACHE_DENTRY);
		dput(sbi->helper_dentry);
		dput(sbi->host_dentry);
		dec(R_ROOT_HOST_DENTRY);
		mntput(sbi->host_mnt);
		dec(R_ROOT_HOST_MNT);
		SBI_VOID(sb) = NULL;
		kfree(sbi);
		dec(R_SBI);
	}
	else
		BUG();

#if 0
	printk("lazyfs: Resource usage after put_super:\n");
	show_resources();
#endif
}

static int
#ifdef LINUX_2_6_SERIES
lazyfs_statfs(struct super_block *sb, struct kstatfs *buf)
#else
lazyfs_statfs(struct super_block *sb, struct statfs *buf)
#endif
{
	buf->f_type = LAZYFS_MAGIC;
	buf->f_bsize = 1024;
	buf->f_bfree = buf->f_bavail = buf->f_ffree;
	buf->f_blocks = 100;
	buf->f_namelen = 1024;

	return 0;
}

/* dentry is a negative dentry. We create a new directory with that name and
 * check to see if it's valid. When the helper does reply, we can delete the
 * directory then if it was a mistake. Other callers can see the directory
 * while this is going on, which is a bit odd.
 */
static inline struct dentry *
lookup_via_helper(struct super_block *sb, struct dentry *dentry)
{
	struct dentry *existing, *new;
	int err;

	down(&update_dir);

	existing = d_lookup(dentry->d_parent, &dentry->d_name);
	if (existing && existing->d_inode) {
		up(&update_dir);
		return existing;
	} else if (existing) {
		printk("lookup_via_helper: freeing\n");
		dput(existing);
	}

	new = dget(new_dentry(sb, dentry->d_parent, dentry->d_name.name,
			S_IFDIR | 0555, 0, CURRENT_TIME, NULL));
	
	up(&update_dir);

	/* Don't block readers while we check it exists... */
	up(&dentry->d_parent->d_inode->i_sem);
	err = ensure_cached(new);
	down(&dentry->d_parent->d_inode->i_sem);

	if (err) {
		dput(new);
		return ERR_PTR(err);
	}

	return new;
}

/* Returning NULL is the same as returning dentry */
static struct dentry *
#ifdef LINUX_2_6_SERIES
lazyfs_lookup(struct inode *dir,struct dentry *dentry, struct nameidata *ni)
#else
lazyfs_lookup(struct inode *dir, struct dentry *dentry)
#endif
{
	struct lazy_de_info *parent_info;
	struct dentry *new;
	int err;

	/* The root "..." file might not exist, but these two still need to be
	 * accessible...
	 */
	if (dir == dir->i_sb->s_root->d_inode) {
		struct lazy_sb_info *sbi = SBI(dir->i_sb);

		if (strcmp(dentry->d_name.name, ".lazyfs-helper") == 0)
			return dget(sbi->helper_dentry);
		else if (strcmp(dentry->d_name.name, ".lazyfs-cache") == 0)
			return dget(sbi->cache_dentry);
	}

	/* Do we still need this now we have d_revalidate? */
	err = ensure_cached(dentry->d_parent);
	if (err)
		return ERR_PTR(err);

	new = d_lookup(dentry->d_parent, &dentry->d_name);
	if (new && new->d_inode)
		return new;	/* Success! */

	parent_info = dentry->d_parent->d_fsdata;
	if (parent_info->dynamic) {
		new = lookup_via_helper(dir->i_sb, dentry);
	}

	if (!new)
		d_add(dentry, NULL);
	
	return new;
}

static int
lazyfs_handle_release(struct inode *inode, struct file *file)
{
	struct lazy_user_request *request = file->private_data;

	//printk("Helper finished handling '%s'\n",
	//			request->dentry->d_name.name);

	down(&fetching_lock);
	destroy_request(request);
	up(&fetching_lock);

	dec(R_REQUEST);

	wake_up_interruptible(&lazy_wait);

	return 0;
}

/* Reading from a request handle gives the path of the file to create */
static int
lazyfs_handle_read(struct file *file, char *buffer, size_t count, loff_t *off)
{
	struct dentry *this;
	struct dentry *last = file->f_dentry->d_inode->i_sb->s_root;
	size_t start_count = count;
	int err;

	if (file->f_dentry == last) {
		if (count < 2)
			return -ENAMETOOLONG;
		err = copy_to_user(buffer, "/\0", 2);
		if (err) return err;
		return 2;
	}

	while (1) {
		this = file->f_dentry;
		if (this == last)
			break;
		/* Find the topmost dir which we haven't written out yet */
		while (this->d_parent != last) {
			if (this == this->d_parent)
				return -ENOTDIR;	/* No longer exists */
			this = this->d_parent;
		}

		if (count < 1)
			return -ENAMETOOLONG;
		err = copy_to_user(buffer, "/", 1);
		if (err) return err;
		count--;
		buffer++;

		if (this->d_name.len > count)
			return -ENAMETOOLONG;

		err = copy_to_user(buffer, this->d_name.name, this->d_name.len);
		if (err) return err;
		buffer += this->d_name.len;
		count -= this->d_name.len;

		last = this;
	}

	if (count < 1)
		return -ENAMETOOLONG;
	err = copy_to_user(buffer, "\0", 1);
	if (err) return err;
	buffer++;
	count--;

	return start_count - count;
}

/* put_filp is no longer exported from 2.6.10.
 * On such kernels, we use fput instead. We need a valid dentry
 * so fput() can examine the inode and dput() it. Inode mustn't be
 * a device, or that gets put too.
 */
static void my_put_filp(struct dentry *dummy, struct file *file)
{
	BUG_ON(dummy == NULL);
	BUG_ON(dummy->d_inode == NULL);
	BUG_ON(dummy->d_inode->i_cdev != NULL);
	BUG_ON(file->f_dentry != NULL);

#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 9)
	file->f_dentry = dget(dummy);
	fput(file);
#else
	put_filp(file);
#endif
}

/* We create a new file object and pass it to userspace. When closed, we
 * check to see if the host has been created. If we don't return an error
 * then lazyfs_handle_release will get called eventually for this dentry...
 */
static ssize_t
send_to_helper(char *buffer, size_t count, struct lazy_user_request *request)
{
	struct dentry *dentry = request->dentry;
	struct super_block *sb = dentry->d_inode->i_sb;
	struct lazy_sb_info *sbi = SBI(sb);
	struct file *file;
	char number[40];
	int len;
	int fd;
	int err = 0;

	//printk("Sending '%s' to helper\n", dentry->d_name.name);

	file = get_empty_filp();
	if (!file)
		return -ENOMEM;
	inc(R_REQUEST);
	file->private_data = NULL;

	fd = get_unused_fd();
	if (fd < 0) {
		my_put_filp(sb->s_root, file);
		return fd;
	}
	len = snprintf(number, sizeof(number), "%d uid=%d", fd, request->uid);
	if (len < 0)
		BUG();
	if (((unsigned int) len) >= count)
		err = -E2BIG;
	else
		err = copy_to_user(buffer, number, len + 1);
	if (err)
	{
		my_put_filp(sb->s_root, file);
		put_unused_fd(fd);
		return err;
	}

	file->f_vfsmnt = mntget(sbi->helper_mnt);
	file->f_dentry = dget(dentry);

	/* At this point, the only way for the request object to be destroyed
	 * is if we return an error, or the helper closes the handle. Either
	 * way, the file dies first, so no need for a ref count.
	 */
	file->private_data = request;

	file->f_pos = 0;
	file->f_flags = O_RDONLY;
	file->f_op = &lazyfs_handle_operations;
	file->f_mode = 1;
	file->f_version = 0;

	fd_install(fd, file);

	return len + 1;
}

static int
lazyfs_helper_open(struct inode *inode, struct file *file)
{
	struct super_block *sb = inode->i_sb;
	struct lazy_sb_info *sbi = SBI(sb);
	int err = 0;

	down(&fetching_lock);
	if (!sbi->helper_mnt) {
		sbi->helper_mnt = mntget(file->f_vfsmnt);
		inc(R_HELPER);
	}
	else
		err = -EBUSY;
	up(&fetching_lock);

	return err;
}

static int
lazyfs_helper_release(struct inode *inode, struct file *file)
{
	struct super_block *sb = inode->i_sb;
	struct lazy_sb_info *sbi = SBI(sb);

	down(&fetching_lock);

	mntput(sbi->helper_mnt);
	dec(R_HELPER);
	sbi->helper_mnt = NULL;	/* Prevents any new requests arriving */

	/* Requests on the pending list will get closed when the handles
	 * are closed, but we have to close requests that haven't been
	 * delivered to the helper yet here...
	 */

	while (!list_empty(&sbi->to_helper)) {
		struct lazy_user_request *request;
		request = list_entry(sbi->to_helper.next,
				     struct lazy_user_request, helper_list);

		destroy_request(request);
	}

	//show_refs(sb->s_root, 0);

	up(&fetching_lock);

	wake_up_interruptible(&lazy_wait);

	return 0;
}

/* Return whether any requests are pending, and add to wait-queue to get
 * notified when there are some (but don't actually wait; do_select does
 * that).
 */
static unsigned int
lazyfs_helper_poll(struct file *file, poll_table *wait)
{
        unsigned int retval = 0;
	struct super_block *sb = file->f_dentry->d_inode->i_sb;
	struct lazy_sb_info *sbi = SBI(sb);

        poll_wait(file, &helper_wait, wait);

	down(&fetching_lock);
	if (!list_empty(&sbi->to_helper))
		retval = POLLIN | POLLRDNORM;
        up(&fetching_lock);

        return retval;
}

static ssize_t
lazyfs_helper_read(struct file *file, char *buffer, size_t count, loff_t *off)
{
	struct super_block *sb = file->f_dentry->d_inode->i_sb;
	struct lazy_sb_info *sbi = SBI(sb);
	int err = 0;
	DECLARE_WAITQUEUE(wait, current);

	add_wait_queue(&helper_wait, &wait);
	do {
		struct lazy_user_request *request = NULL;

		current->state = TASK_INTERRUPTIBLE;

		down(&fetching_lock);
		if (!list_empty(&sbi->to_helper)) {

			request = list_entry(sbi->to_helper.next,
					struct lazy_user_request, helper_list);
			list_move(&request->helper_list,
				  &sbi->requests_in_progress);
		}
		up(&fetching_lock);

		if (request) {
			/*
			if (!find_user_request(request->dentry->d_fsdata,
					  request->uid))
				BUG(); */

			err = send_to_helper(buffer, count, request);

			if (err < 0) {
				/* Failed... error */
				down(&fetching_lock);
				destroy_request(request);
				up(&fetching_lock);
				wake_up_interruptible(&lazy_wait);
			}
			goto out;
		}

		if (signal_pending(current)) {
			err = -ERESTARTSYS;
			goto out;
		}
		schedule();
		if (signal_pending(current)) {
			err = -ERESTARTSYS;
			goto out;
		}
	} while (1);

out:
        current->state = TASK_RUNNING;
        remove_wait_queue(&helper_wait, &wait);

	return err;
}

/*			File operations				*/

/* Try to fill in file->private_data->host_file with the host file.
 * flags are passed directly to get_host_dentry().
 */
static int
get_host_file(struct file *file, int flags)
{
	struct dentry *dentry = file->f_dentry;
	struct super_block *sb = dentry->d_inode->i_sb;
	struct lazy_sb_info *sbi = SBI(sb);
	struct dentry *host_dentry;
	struct file *host_file;
	struct lazy_file_info *finfo = file->private_data;

	if (!finfo)
		BUG();

	//printk("get_host_file(%s): %d\n", dentry->d_name.name, block);
	/* This is just an optimisation; saves getting and freeing the
	 * host dentry.
	 */
	if (finfo->host_file) {
		printk("get_host_file: already set\n");
		return 0;
	}
	
	host_dentry = get_host_dentry(dentry, flags);
	if (IS_ERR(host_dentry))
		return PTR_ERR(host_dentry);

	/* This is another optimisation; saves opening and closing the host
	 * file. This is fairly common, since the above get_host_dentry may
	 * have blocked for a long time.
	 */
	if (finfo->host_file) {
		/* Someone else filled in host_file while we were waiting. */
		//printk("get_host_file(%s): set during get_host_dentry\n",
		//		dentry->d_name.name);
		dput(host_dentry);
		dec(R_HOST_DENTRY);
		return 0;
	}

	/* Open the host file */
	{
		struct vfsmount *mnt = mntget(sbi->host_mnt);
		host_file = dentry_open(host_dentry, mnt, file->f_flags);
		dec(R_HOST_DENTRY);
		host_dentry = NULL;
		/* (mnt and dentry freed by here) */
	}

	if (IS_ERR(host_file))
		return PTR_ERR(host_file);

	spin_lock(&host_file_lock);
	if (finfo->host_file) {
		/* Someone else filled in host_file while we were opening
		 * the host file. Unlikely.
		 */
		spin_unlock(&host_file_lock);
		printk("get_host_file: filled in while opening host file\n");
		fput(host_file);
		return 0;
	}
	//printk("get_host_file(%s): adding\n", dentry->d_name.name);

	BUG_ON(host_file->f_dentry == NULL);

	finfo->host_file = host_file;
	inc(R_FILE_HOST_FILE);
	spin_unlock(&host_file_lock);

	return 0;
}

static ssize_t
lazyfs_file_read(struct file *file, char *buffer, size_t count, loff_t *off)
{
	struct lazy_file_info *finfo = file->private_data;
	struct file *host_file;
	
	if (!finfo->host_file)
	{
		int err;
		err = get_host_file(file,
				    GET_HOST_MAY_BLOCK |
				    GET_HOST_DONT_START);
		if (err)
			return err;
	}
	host_file = finfo->host_file;
	if (!host_file)
		BUG();

	if (host_file->f_op && host_file->f_op->read)
		return host_file->f_op->read(host_file, buffer, count, off);

	return -EINVAL;
}

static int
lazyfs_file_mmap(struct file *file, struct vm_area_struct *vm)
{
	struct lazy_file_info *finfo = file->private_data;
	struct file *host_file;
	struct inode *inode, *host_inode;
	int err;

	if (!finfo->host_file)
	{
		err = get_host_file(file,
				    GET_HOST_MAY_BLOCK |
				    GET_HOST_DONT_START);
		if (err)
			return err;
	}
	host_file = finfo->host_file;
	BUG_ON(host_file == NULL);

	if (!host_file->f_op || !host_file->f_op->mmap)
		return -ENODEV;

	BUG_ON(host_file->f_dentry == NULL);

	inode = file->f_dentry->d_inode;
	host_inode = host_file->f_dentry->d_inode;

	if (inode->i_mapping != &inode->i_data &&
	    inode->i_mapping != host_inode->i_mapping) {
		/* We already forwarded the host mapping, but it's changed.
		 * This should only happen if the host file has been removed
		 * and replaced with another one.
		 */
		return -EBUSY;
	}

	/* Coda does this call last, but I think mmap could change
	 * host_inode->i_mapping (after all, we do!).
	 */
	err = host_file->f_op->mmap(host_file, vm);
	if (err)
		return err;

	/* Make sure the mapping for our inode points to the host file's */
	spin_lock(&mapping_lock);

	(*((int *) &inode->u.generic_ip))++;
	finfo->n_mappings++;
	if (inode->i_mapping == &inode->i_data) {
		if (((int) inode->u.generic_ip) != 1)
			BUG();
		inc(R_MAPPING);
		inode->i_mapping = host_inode->i_mapping;
	}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,2)
	file->f_mapping = inode->i_mapping;
#endif

	spin_unlock(&mapping_lock);

	return 0;
}

static int
lazyfs_link_follow_link(struct dentry *dentry, struct nameidata *nd)
{
	const char *target = dentry->d_inode->u.generic_ip;

	if (target)
		return vfs_follow_link(nd, target);

	printk("lazyfs_link_follow_link: no link target!\n");
	return -EINVAL;
}

static int
lazyfs_link_readlink(struct dentry *dentry, char *buf, int bufsize)
{
	char *target = dentry->d_inode->u.generic_ip;
	int len;
	int err, copy_err;

	if (!target)
		BUG();

	len = strlen(target);
	if (len > bufsize) {
		err = -ENAMETOOLONG;
		len = bufsize;
	}
	else
		err = len;

	copy_err = copy_to_user(buf, target, len);

	return copy_err ? copy_err : err;
}

/* Note: we don't return -EAGAIN, because programs don't except it.
 * Instead, we just fire off a request for the file and return right
 * away. The read() or mmap() call will actually block on the host file
 * becoming available.
 */
static int
lazyfs_file_open(struct inode *inode, struct file *file)
{
	struct lazy_file_info *finfo;
	int err;

	finfo = kmalloc(sizeof(struct lazy_file_info), GFP_KERNEL);
	if (!finfo)
		return -ENOMEM;

	finfo->host_file = NULL;
	finfo->n_mappings = 0;
	file->private_data = finfo;
	
	err = get_host_file(file, 0);
	if (err == -EAGAIN)
		err = 0;	/* We'll pick it up in the read */

	if (err)
		kfree(finfo);

	return err;
}

static int
lazyfs_file_release(struct inode *inode, struct file *file)
{
	struct lazy_file_info *finfo = file->private_data;
	struct file *host_file;
	
	if (!finfo)
		BUG();

	host_file = finfo->host_file;

	if (!host_file)
		goto out;	/* Opened but never read */

	if (finfo->n_mappings) {
		//printk("File was mmapped %d times\n", finfo->n_mappings);

		spin_lock(&mapping_lock);

		*((int *) &inode->u.generic_ip) -= finfo->n_mappings;

		if (((int) inode->u.generic_ip) == 0) {
			//printk("Last mapping gone!\n");
			inode->i_mapping = &inode->i_data;
			dec(R_MAPPING);
		}

		spin_unlock(&mapping_lock);
	}

	fput(host_file);
	dec(R_FILE_HOST_FILE);

out:
	kfree(finfo);
	return 0;
}

/* Return whether this file is readable (whether reading would block). */
static unsigned int
lazyfs_file_poll(struct file *file, poll_table *wait)
{
        unsigned int retval = 0;

        poll_wait(file, &lazy_wait, wait);

	if (get_host_file(file, GET_HOST_DONT_START) != -EAGAIN) {
		retval = POLLIN | POLLRDNORM;
	}

        return retval;
}

static int
lazyfs_dir_open(struct inode *inode, struct file *file)
{
	int err;

	if (file->f_dentry->d_parent != file->f_dentry &&
	    d_unhashed(file->f_dentry)) {
		/* Directory no longer exists */
		return dcache_dir_open(inode, file);
	}

	err = ensure_cached(file->f_dentry);

	return err ? err : dcache_dir_open(inode, file);
}

static int
lazyfs_unmapped_readpage(struct file *file, struct page *page)
{
	printk("lazyfs: Attempt to call readpage on an unmapped file ('%s')\n"
	       "Did you upgrade your kernel? Please recompile lazyfs if so.\n",
			file->f_dentry->d_name.name);
	return -ENOSYS;
}


/*			Classes					*/

static struct super_operations lazyfs_ops = {
	statfs:		lazyfs_statfs,
	put_super:	lazyfs_put_super,
	clear_inode:	lazyfs_clear_inode,
};

static struct inode_operations lazyfs_dir_inode_operations = {
	lookup:		lazyfs_lookup,
};

static struct file_operations lazyfs_helper_operations = {
	open:		lazyfs_helper_open,
	read:		lazyfs_helper_read,
	release:	lazyfs_helper_release,
	poll:		lazyfs_helper_poll,
};

static struct file_operations lazyfs_handle_operations = {
	read:		lazyfs_handle_read,
	release:	lazyfs_handle_release,
};

static struct inode_operations lazyfs_link_operations = {
	readlink:	lazyfs_link_readlink,
	follow_link:	lazyfs_link_follow_link,
};

static struct file_operations lazyfs_file_operations = {
	open:		lazyfs_file_open,
	read:		lazyfs_file_read,
	mmap:		lazyfs_file_mmap,
	release:	lazyfs_file_release,
	poll:		lazyfs_file_poll,
};

static struct dentry_operations lazyfs_dentry_ops = {
	d_release:	lazyfs_release_dentry,
	d_revalidate:	lazyfs_dentry_revalidate,
};

static struct file_operations lazyfs_dir_file_operations = {
	open:		lazyfs_dir_open,
	release:	dcache_dir_close,
	llseek:		dcache_dir_lseek,
	read:		generic_read_dir,
	readdir:	dcache_readdir,
};

#ifdef LINUX_2_6_SERIES
struct file_system_type lazyfs_fs_type = {
	.name		= "lazyfs" COMMA_VERSION,
	.get_sb		= lazyfs_get_sb,
	.fs_flags	= 0,
	.owner		= THIS_MODULE,
	.kill_sb	= kill_litter_super,
};
#else
static DECLARE_FSTYPE(lazyfs_fs_type, "lazyfs" COMMA_VERSION,
			lazyfs_read_super, FS_LITTER);
#endif

static struct address_space_operations lazyfs_unmapped_aops = {
	.readpage	= lazyfs_unmapped_readpage,
};

static int __init init_lazyfs_fs(void)
{
	init_resources();
	return register_filesystem(&lazyfs_fs_type);
}

static void __exit exit_lazyfs_fs(void)
{
	unregister_filesystem(&lazyfs_fs_type);
}

#ifndef LINUX_2_6_SERIES
EXPORT_NO_SYMBOLS;
#endif

module_init(init_lazyfs_fs)
module_exit(exit_lazyfs_fs)
MODULE_LICENSE("GPL");
