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


	/* WARNING: This code is quite new. It may contain bugs, so don't
	 * run it on anything too important. Please audit and/or stress
	 * test it and report any bugs!
	 */

/* See the 'Technical' file for details. */

/* Locking:
 * After the initial read_super, the virtual directory tree can only be changed
 * from within ensure_cached(), which uses a static lock to prevent races.
 */

#include <linux/module.h>   /* Needed by all modules */
#include <linux/kernel.h>   /* Needed for KERN_ALERT */
#include <linux/init.h>     /* Needed for the macros */

#if CONFIG_MODVERSIONS==1
#define MODVERSIONS
#include <linux/modversions.h>
#endif

#define LAZYFS_MAX_LISTING_SIZE (100*1024)

#include <linux/module.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/locks.h>
#include <linux/init.h>
#include <linux/smp_lock.h>
#include <linux/file.h>

#include <asm/uaccess.h>

#include "lazyfs.h"

#if 1	/* Debugging */
static atomic_t resource_counter = ATOMIC_INIT(0);
static inline void inc(char *m)
{
	//printk("+ %s\n", m);
	atomic_inc(&resource_counter);
}
static inline void dec(char *m)
{
	//printk("- %s\n", m);
	atomic_dec(&resource_counter);
}
static void show_refs(struct dentry *dentry, int indent)
{
	struct list_head *next;
	int i;

	for (i = 0; i < indent; i++)
		printk(" ");
	printk("'%s' [%d] %s %s\n", dentry->d_name.name,
			atomic_read(&dentry->d_count),
			indent != 0 && d_unhashed(dentry) ? "(unhashed)" : "",
			!dentry->d_inode ? "(negative)" : "");

	next = dentry->d_subdirs.next;
	while (next != &dentry->d_subdirs) {
		struct dentry *c = list_entry(next, struct dentry, d_child);
		show_refs(c, indent + 2);
		next = next->next;
	}
}
#endif

//static void show_refs(struct dentry *dentry, int indent);

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
struct lazy_sb_info
{
	struct file *host_file;	/* The root of the cache, passed in mount */
	struct dentry *helper_dentry;	/* The .lazyfs-helper dentry */

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
struct lazy_de_info
{
	struct dentry *dentry;	/* dentry->d_fsdata->dentry == dentry */
	int may_delete;		/* Used during mark-and-sweep */

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

	/* An info block may be in three states:
	 * - to_helper empty : file is not being fetched.
	 * - in sbi->to_helper : about to be sent to helper.
	 * - in sbi->requests_in_progress: being handled by helper.
	 * Protected by fetching_lock.
	 */
	struct list_head helper_list;
};

/* Any change to the helper_list queues requires this lock */
spinlock_t fetching_lock = SPIN_LOCK_UNLOCKED;

static struct super_operations lazyfs_ops;
static struct file_operations lazyfs_dir_operations;
static struct inode_operations lazyfs_dir_inode_operations;
static struct dentry_operations lazyfs_dentry_ops;
static struct file_operations lazyfs_file_operations;
static struct file_operations lazyfs_helper_operations;
static struct file_operations lazyfs_handle_operations;
static struct inode_operations lazyfs_link_operations;

static int ensure_cached(struct dentry *dentry);

/* Symlinks store their target in the inode. Free that here. */
static void
lazyfs_put_inode(struct inode *inode)
{
	if (S_ISLNK(inode->i_mode) && inode->u.generic_ip) {
		kfree(inode->u.generic_ip);
		dec("target");
	}
}

static void
lazyfs_release_dentry(struct dentry *dentry)
{
	struct lazy_de_info *info = dentry->d_fsdata;
	
	if (dentry->d_inode)
		BUG();
	if (!info)
		BUG();
	if (!list_empty(&info->helper_list))
		BUG();

	if (info->host_dentry) {
		dec("info->host_dentry");
		dput(info->host_dentry);
	}
	if (info->list_dentry) {
		dec("info->list_dentry");
		dput(info->list_dentry);
	}

	dentry->d_fsdata = NULL;
	kfree(info);
	dec("dentry_info");
}

/* Create a new inode with an unused inode number */
static inline struct inode *
lazyfs_new_inode(struct super_block *sb, mode_t mode,
		 loff_t size, time_t mtime, struct qstr *link_target)
{
	struct inode *inode;

	inode = new_inode(sb);
	if (!inode)
		return NULL;
	inode->u.generic_ip = NULL;

	inode->i_mode = mode | 0444;	/* Always give read */
	inode->i_nlink = 1;
	inode->i_uid = 0;
	inode->i_gid = 0;
	inode->i_size = size;
	inode->i_atime = CURRENT_TIME;
	inode->i_ctime = inode->i_mtime = mtime;
	if (S_ISDIR(mode)) {
		inode->i_op = &lazyfs_dir_inode_operations;
		inode->i_fop = &lazyfs_dir_operations;
	}
	else if (S_ISREG(mode))
		inode->i_fop = &lazyfs_file_operations;
	else if (S_ISLNK(mode)) {
		char *target;
		target = kmalloc(link_target->len + 1, GFP_KERNEL);
		if (!target) {
			iput(inode);
			return NULL;
		}
		inc("target");
		memcpy(target, link_target->name, link_target->len);
		target[link_target->len] = '\0';
		inode->u.generic_ip = target;

		inode->i_op = &lazyfs_link_operations;
	}
	else
		BUG();
	return inode;
}

/* If parent_dentry is NULL, then this creates a new root dentry,
 * otherwise, refs the parent.
 * Only created during init an when opening a directory. Must put a lock
 * around the whole open_dir operation.
 */
static struct dentry *
new_dentry(struct super_block *sb, struct dentry *parent_dentry,
	   const char *leaf, mode_t mode, loff_t size, time_t mtime,
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
	inc("dentry_info");
	new->d_op = &lazyfs_dentry_ops;
	new->d_fsdata = info;
	info->dentry = new;
	info->host_dentry = NULL;
	info->list_dentry = NULL;
	info->may_delete = 0;
	INIT_LIST_HEAD(&info->helper_list);

	if (parent_dentry)
		d_add(new, inode);

	return new;
err:
	iput(inode);
	if (new)
		dput(new);
	return NULL;
}

/* A file descriptor is passed to the mount command. Get the file from it. */
static inline struct file *
file_from_mount_data(struct lazy_mount_data *mount_data)
{
	struct file *file;
	struct inode *inode = NULL;

	if (!mount_data) {
		printk("lazyfs_read_super: Bad mount data\n");
		return NULL;
	}
	if (mount_data->version != 1) {
		printk("lazyfs_read_super: Wrong version number\n");
		return NULL;
	}

	file = fget(mount_data->fd);
	if (file)
		inode = file->f_dentry->d_inode;
	if (!inode || !S_ISDIR(inode->i_mode)) {
		printk("lazyfs_read_super: Bad file\n");
		fput(file);
		return NULL;
	}
	inc("mount host file");
	return file;
}

static struct super_block *
lazyfs_read_super(struct super_block *sb, void *data, int silent)
{
	struct lazy_sb_info *sbi = NULL;

	sbi = kmalloc(sizeof(struct lazy_sb_info), GFP_KERNEL);
	if (!sbi)
		return NULL;
	sb->u.generic_sbp = sbi;
	inc("sbi");

	sbi->host_file = file_from_mount_data((struct lazy_mount_data *) data);
	if (!sbi->host_file)
		goto err;

	sb->s_blocksize = 1024;
	sb->s_blocksize_bits = 10;
	sb->s_magic = LAZYFS_MAGIC;
	sb->s_op = &lazyfs_ops;

	sb->s_root = new_dentry(sb, NULL, "/", S_IFDIR | 0111, 0,
			sbi->host_file->f_dentry->d_inode->i_mtime, NULL);
	if (!sb->s_root)
		goto err;

	sbi->helper_dentry = new_dentry(sb, sb->s_root, ".lazyfs-helper",
			S_IFREG, 0, CURRENT_TIME, NULL);
	if (!sbi->helper_dentry)
		goto err;
	sbi->helper_dentry->d_inode->i_fop = &lazyfs_helper_operations;
	sbi->helper_dentry->d_inode->i_mode = S_IFREG | 0600;
	sbi->helper_mnt = NULL;
	INIT_LIST_HEAD(&sbi->to_helper);
	INIT_LIST_HEAD(&sbi->requests_in_progress);

	return sb;
err:
	if (sbi->host_file) {
		fput(sbi->host_file);
		dec("mount host file");
	}
	if (sbi) {
		kfree(sbi);
		dec("sbi");
	}
	if (sb->s_root)
		dput(sb->s_root);
	return NULL;
}

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
	inc("list_dentry");
	if (!list_dentry->d_inode)
		goto fetch;
	if (!S_ISREG(list_dentry->d_inode->i_mode))
		goto fetch;

	/* Cache host_dentry */
	old = info->host_dentry;
	info->host_dentry = dget(host_dentry);
	inc("info->host_dentry");
	if (old) {
		dput(old);
		dec("info->host_dentry");
	}

	return list_dentry;
fetch:
	dec("list_dentry");
	dput(list_dentry);
	return NULL;
}

/* Try to open parent_host/dentry.name. If it works, and it has the correct
 * type, and (for directories) has a '...' file, return it (or the '...').
 *
 * Returns NULL if there is no error, but the host does not exist yet, or
 * if it exists, but is the wrong type of object.
 *
 * If parent_host is NULL, get the root of the cache.
 */
static struct dentry *try_get_host_dentry(struct dentry *dentry,
					  struct dentry *parent_host)
{
	struct dentry *host_dentry;
	mode_t mode, host_mode;
	struct qstr name = dentry->d_name;	/* Copy, for hash */
	struct super_block *sb = dentry->d_inode->i_sb;

	if ((!parent_host) != (dentry == sb->s_root))
		BUG();
	
	if (!parent_host) {
		struct lazy_sb_info *sbi = sb->u.generic_sbp;
		host_dentry = dget(sbi->host_file->f_dentry);
	} else {
		down(&parent_host->d_inode->i_sem);
		host_dentry = lookup_hash(&name, parent_host);
		up(&parent_host->d_inode->i_sem);
	}

	if (IS_ERR(host_dentry))
		return host_dentry;
	inc("host_dentry");

	if (!host_dentry)
		BUG();

	if (!host_dentry->d_inode)
		goto fetch;

	mode = dentry->d_inode->i_mode;
	host_mode = host_dentry->d_inode->i_mode;

	if (S_ISREG(mode)) {
		if (S_ISREG(host_mode))
			return host_dentry;	/* File, OK */
		goto fetch;
	} else if (S_ISDIR(mode)) {
		struct dentry *list_dentry;

		if (!S_ISDIR(host_mode))
			goto fetch;

		list_dentry = get_host_dir_dentry(dentry, host_dentry);
		dput(host_dentry);
		dec("host_dentry");

		return list_dentry;
	}

	BUG();

fetch:
	/* host_dentry exists, but is negative or the wrong type */
	dput(host_dentry);
	dec("host_dentry");
	return NULL;
}

/* Return the dentry for this host. It's parent directory must already have
 * one. If the host inode does not yet exist, it sleeps until the helper
 * creates it (if blocking is 1).
 * If 'dentry' is a directory, then the returned value will be the dentry
 * of the '...' file. The host_dentry for the directory itself is cached.
 */
static struct dentry *get_host_dentry(struct dentry *dentry, int blocking)
{
	struct super_block *sb = dentry->d_inode->i_sb;
	struct lazy_sb_info *sbi = sb->u.generic_sbp;
	struct dentry *host_dentry;
	struct dentry *parent_host = NULL;
	struct lazy_de_info *info = dentry->d_fsdata;
	int first_try = 1;
	DECLARE_WAITQUEUE(wait, current);

	if (!info)
		BUG();

	if (dentry != sb->s_root) {
		struct lazy_de_info *parent_info = dentry->d_parent->d_fsdata;

		/* parent_host can't be NULL, but it can change under us */
		parent_host = dget(parent_info->host_dentry);
		if (!parent_host)
			BUG();
		inc("parent_host");
	}

	add_wait_queue(&lazy_wait, &wait);
	do {
		int start_fetching = 0;
		int had_helper = 0;

		host_dentry = try_get_host_dentry(dentry, parent_host);
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
		spin_lock(&fetching_lock);
		had_helper = sbi->helper_mnt != NULL;
		if (had_helper && list_empty(&info->helper_list)) {
			dget(dentry);
			list_add_tail(&info->helper_list, &sbi->to_helper);
			start_fetching = 1;
		}
		spin_unlock(&fetching_lock);
		host_dentry = ERR_PTR(-EIO);
		if (!had_helper)
			goto out;
		if (start_fetching)
			wake_up_interruptible(&helper_wait);
		/* else someone else is already fetching it */

		host_dentry = ERR_PTR(-EAGAIN);
		if (!blocking)
                        goto out;

		while (1) {
			current->state = TASK_INTERRUPTIBLE;
			spin_lock(&fetching_lock);
			if (list_empty(&info->helper_list)) {
				spin_unlock(&fetching_lock);
				break;	/* Fetch request completed */
			}
			spin_unlock(&fetching_lock);

			if (signal_pending(current)) {
				host_dentry = ERR_PTR(-ERESTARTSYS);
				goto out;
			}
			
			schedule();
		}
	} while (1);

out:
	if (parent_host) {
		dput(parent_host);
		dec("parent_host");
	}
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
	struct lazy_sb_info *sbi = sb->u.generic_sbp;
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

		if (child != sbi->helper_dentry)
			info->may_delete = 1;
	}

	spin_unlock(&dcache_lock);
}

/* Problem: We can't delete a subtree if any of the dentries
 * in it are still being used. But, we can't rely on a later dput
 * to remove the subtree either.
 * Try turning every node into a separate tree...
 */
static void my_d_genocide(struct dentry *root)
{
	struct dentry *this_parent = root;
	struct list_head *next;

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
		atomic_dec(&dentry->d_count);
		atomic_dec(&dentry->d_parent->d_count);
		dentry->d_parent = dentry;
	}
	if (this_parent != root) {
		next = this_parent->d_child.next; 
		atomic_dec(&this_parent->d_count);
		this_parent = this_parent->d_parent;
		goto resume;
	}
	spin_unlock(&dcache_lock);
}

/* Removes dentry from the tree, and any child nodes too.
 * Note: removes the ref that the tree held, but doesn't drop the reference
 * passed in.
 */
static void remove_dentry(struct dentry *dentry)
{
	my_d_genocide(dentry);
	d_delete(dentry);
	dput(dentry);
}

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
		remove_dentry(child);
		goto restart;
	}

	spin_unlock(&dcache_lock);

	shrink_dcache_parent(dentry);
}

static inline int
has_changed(struct inode *i, mode_t mode, size_t size, time_t time,
	    struct qstr *link_target)
{
	if (i->i_mode != mode || i->i_size != size || i->i_mtime != time)
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
	const char *end = listing + size;

	/* Wipe the dcache below this point and reassert this directory's new
	 * contents into it. The rest of the tree will be rebuilt on demand, as
	 * usual. Note that all the inode numbers change when we do this, even
	 * if the new information is the same as before.
	 */

	/* Check for the magic string */
	if (size < 7 || strncmp(listing, "LazyFS\n", 7) != 0)
		return -EIO;
	listing += 7;

	mark_children_may_delete(dir);

	while (listing < end) {
		struct dentry *existing;
		mode_t mode = 0444;
		struct qstr name;
		struct qstr link_target;
		off_t size;
		time_t time;

		switch (*(listing++)) {
			case 'f': mode |= S_IFREG; break;
			case 'x': mode |= S_IFREG | 0111; break;
			case 'd': mode |= S_IFDIR | 0111; break;
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
		time = atoi(&listing);
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

		name.hash = full_name_hash(name.name, name.len);
		existing = d_lookup(dir, &name);
		if (existing && existing->d_inode) {
			struct inode *i = existing->d_inode;
			if (has_changed(i, mode, size, time, &link_target)) {
				remove_dentry(existing);
				new_dentry(sb, dir, name.name,
					   mode, size, time, &link_target);
			} else {
				struct lazy_de_info *info = existing->d_fsdata;
				info->may_delete = 0;
			}
		} else
			new_dentry(sb, dir, name.name, mode, size, time,
					&link_target);
		if (existing)
			dput(existing);
	}

	sweep_marked_children(dir);

	if (listing != end)
		BUG();

	//show_refs(dir->d_inode->i_sb->s_root, 0);
	
	return 0;

bad_list:
	printk("lazyfs: '%s/...' file is invalid\n", dir->d_name.name);

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
	file = dentry_open(dget(dentry), mntget(mnt), 1);
	if (IS_ERR(file))
		return (char *) file;
	inc("ddd file");

	size = file->f_dentry->d_inode->i_size;
	if (size < 0 || size > max_size) {
		printk("lazyfs: file too big\n");
		fput(file);
		dec("ddd file");
		return ERR_PTR(-E2BIG);
	}

	listing = kmalloc(size + 1, GFP_KERNEL);
	if (!listing) {
		fput(file);
		dec("ddd file");
		return ERR_PTR(-ENOMEM);
	}
	listing[size] = '\0';
	inc("listing");

	while (offset < size) {
		int got;

		got = kernel_read(file, offset, listing + offset,
				  size - offset);
		if (got <= 0) {
			printk("lazyfs: error reading file\n");
			fput(file);
			dec("ddd file");
			kfree(listing);
			dec("listing");
			return ERR_PTR(got < 0 ? got : -EIO);
		}
		offset += got;
	}
	fput(file);
	dec("ddd file");

	if (offset != size)
		BUG();

	*size_out = size;
	return listing;
}

/* Make sure the dcache relects the contents of '...'. If '...' is
 * missing, try to fetch it now.
 */
static int ensure_cached(struct dentry *dentry)
{
	static spinlock_t update_dir = SPIN_LOCK_UNLOCKED;
	struct lazy_de_info *info = dentry->d_fsdata;
	struct dentry *list_dentry = NULL;
	int err;
	char *listing;
	off_t size;
	struct super_block *sb = dentry->d_inode->i_sb;
	struct lazy_sb_info *sbi = sb->u.generic_sbp;

	if (!S_ISDIR(dentry->d_inode->i_mode))
		BUG();

	list_dentry = get_host_dentry(dentry, 1);
	if (IS_ERR(list_dentry))
		return PTR_ERR(list_dentry);
	
	spin_lock(&update_dir);
	if (list_dentry == info->list_dentry) {
		/* Already cached... do nothing */
		spin_unlock(&update_dir);
		dput(list_dentry);
		dec("list_dentry");
		return 0;
	}

	/* Switch to the new version */
	if (info->list_dentry) {
		dput(info->list_dentry);
		dec("info->list_dentry");
	}
	info->list_dentry = dget(list_dentry);
	inc("info->list_dentry");

	listing = open_and_read(list_dentry, sbi->host_file->f_vfsmnt,
				LAZYFS_MAX_LISTING_SIZE, &size);
	dput(list_dentry);
	dec("list_dentry");
	if (IS_ERR(listing)) {
		spin_unlock(&update_dir);
		return PTR_ERR(listing);
	}
	
	err = add_dentries_from_list(dentry, listing, size);

	kfree(listing);
	dec("listing");

	if (err == -EIO) {
		struct inode *dir;
		list_dentry = dget(info->list_dentry);
		inc("list_dentry");
		dir = list_dentry->d_parent->d_inode;
		/* File is corrupted. Delete it. */
		down(&dir->i_sem);
		if (vfs_unlink(dir, list_dentry))
			printk("Error unlinking broken ... file!\n");
		up(&dir->i_sem);
		dput(list_dentry);
		dec("list_dentry");
	}

	spin_unlock(&update_dir);

	return err;
}

static int
lazyfs_dir_open(struct inode *inode, struct file *file)
{
	return ensure_cached(file->f_dentry);
}

static void
lazyfs_put_super(struct super_block *sb)
{
	struct lazy_sb_info *sbi = sb->u.generic_sbp;

	if (sbi) {
		if (!list_empty(&sbi->to_helper))
			BUG();
		if (!list_empty(&sbi->requests_in_progress))
			BUG();
		if (sbi->helper_mnt)
			BUG();
		
		dput(sbi->helper_dentry);
		fput(sbi->host_file);
		dec("mount host file");
		sb->u.generic_sbp = NULL;
		kfree(sbi);
		dec("sbi");
	}
	else
		BUG();

	printk("Resource usage after put_super: %d\n",
			atomic_read(&resource_counter));
}

static int
lazyfs_statfs(struct super_block *sb, struct statfs *buf)
{
	buf->f_type = LAZYFS_MAGIC;
	buf->f_bsize = 1024;
	buf->f_bfree = buf->f_bavail = buf->f_ffree;
	buf->f_blocks = 100;
	buf->f_namelen = 1024;
	return 0;
}

static int
lazyfs_readdir(struct file *file, void *dirent, filldir_t filldir)
{
	struct dentry *dir = file->f_dentry; /* (the virtual dir) */
	struct list_head *head, *next;
	int skip = file->f_pos;
	int count = 0, err = 0;

	if (skip)
		skip--;
	else {
		err = filldir(dirent, ".", 1, 0, dir->d_inode->i_ino, DT_DIR);
		if (err)
			return count ? count : err;
		file->f_pos++;
		count++;
	}

	if (skip)
		skip--;
	else {
		err = filldir(dirent, "..", 2, 1,
				dir->d_parent->d_inode->i_ino, DT_DIR);
		if (err)
			return count ? count : err;
		file->f_pos++;
		count++;
	}

	/* Open should have made sure the directory is up-to-date, so
	 * just read out directly from the dircache.
	 */
	spin_lock(&dcache_lock);
	head = &file->f_dentry->d_subdirs;
	next = head->next;

	while (next != head) {
		struct dentry *child = list_entry(next, struct dentry, d_child);
		mode_t mode = child->d_inode->i_mode;

		next = next->next;

		if (d_unhashed(child)||!child->d_inode)
			continue;

		if (skip) {
			skip--;
			continue;
		}

		file->f_pos++;
		err = filldir(dirent, child->d_name.name,
				      child->d_name.len,
			      file->f_pos,
			      child->d_inode->i_ino,
			      S_ISDIR(mode) ? DT_DIR :
			      S_ISLNK(mode) ? DT_LNK :
			      S_ISREG(mode) ? DT_REG : DT_UNKNOWN);
		if (err)
			goto out;
		count++;
	}
out:
	spin_unlock(&dcache_lock);
	return count ? count : err;
}

/* Returning NULL is the same as returning dentry */
static struct dentry *
lazyfs_lookup(struct inode *dir, struct dentry *dentry)
{
	struct dentry *new;

	if (dir == dir->i_sb->s_root->d_inode &&
		strcmp(dentry->d_name.name, ".lazyfs-helper") == 0) {
		struct lazy_sb_info *sbi = dir->i_sb->u.generic_sbp;
		return dget(sbi->helper_dentry);
	}

	ensure_cached(dentry->d_parent);
	new = d_lookup(dentry->d_parent, &dentry->d_name);
	if (!new)
		d_add(dentry, NULL);
	return new;
}

static int
lazyfs_handle_release(struct inode *inode, struct file *file)
{
	struct dentry *dentry = file->f_dentry;
	struct lazy_de_info *info = dentry->d_fsdata;

	if (!info)
		BUG();

	//printk("Helper finished handling '%s'\n", dentry->d_name.name);

	spin_lock(&fetching_lock);
	if (list_empty(&info->helper_list))
		BUG();
	list_del_init(&info->helper_list);
	spin_unlock(&fetching_lock);

	dec("request");

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
		if (count < 1)
			return -ENAMETOOLONG;
		err = copy_to_user(buffer, "/", 1);
		if (err) return err;
		return 1;
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

/* We create a new file object and pass it to userspace. When closed, we
 * check to see if the host has been created. If we don't return an error
 * then lazyfs_handle_release will get called eventually for this dentry...
 */
static ssize_t
send_to_helper(char *buffer, size_t count, struct dentry *dentry)
{
	struct super_block *sb = dentry->d_inode->i_sb;
	struct lazy_sb_info *sbi = sb->u.generic_sbp;
	struct file *file;
	char number[20];
	int len = dentry->d_name.len;
	int fd;
	int err = 0;

	//printk("Sending '%s' to helper\n", dentry->d_name.name);

	file = get_empty_filp();
	if (!file)
		return -ENOMEM;
	inc("request");
	fd = get_unused_fd();
	if (fd < 0) {
		fput(file);
		return fd;
	}
	len = snprintf(number, sizeof(number), "%d", fd);
	if (len < 0)
		BUG();

	file->f_vfsmnt = mntget(sbi->helper_mnt);
	file->f_dentry = dget(dentry);

	file->f_pos = 0;
	file->f_flags = O_RDONLY;
	file->f_op = &lazyfs_handle_operations;
	file->f_mode = 1;
	file->f_version = 0;

	fd_install(fd, file);

	err = copy_to_user(buffer, number, len + 1);

	return err ? err : len + 1;
}

static int
lazyfs_helper_open(struct inode *inode, struct file *file)
{
	struct super_block *sb = inode->i_sb;
	struct lazy_sb_info *sbi = sb->u.generic_sbp;
	int err = 0;

	spin_lock(&fetching_lock);
	if (!sbi->helper_mnt) {
		sbi->helper_mnt = mntget(file->f_vfsmnt);
		inc("helper");
	}
	else
		err = -EBUSY;
	spin_unlock(&fetching_lock);

	return err;
}

static int
lazyfs_helper_release(struct inode *inode, struct file *file)
{
	struct super_block *sb = inode->i_sb;
	struct lazy_sb_info *sbi = sb->u.generic_sbp;

	spin_lock(&fetching_lock);

	mntput(sbi->helper_mnt);
	dec("helper");
	sbi->helper_mnt = NULL;	/* Prevents any new requests arriving */

	while (!list_empty(&sbi->to_helper)) {
		struct lazy_de_info *info;
		info = list_entry(sbi->to_helper.next,
				struct lazy_de_info, helper_list);

		list_del_init(&info->helper_list);

		printk("Discarding '%s'\n", info->dentry->d_name.name);
	}

	spin_unlock(&fetching_lock);

	wake_up_interruptible(&lazy_wait);

	return 0;
}

static ssize_t
lazyfs_helper_read(struct file *file, char *buffer, size_t count, loff_t *off)
{
	struct super_block *sb = file->f_dentry->d_inode->i_sb;
	struct lazy_sb_info *sbi = sb->u.generic_sbp;
	int err = 0;
	DECLARE_WAITQUEUE(wait, current);

	if (count < 4)
		return -EINVAL;

	add_wait_queue(&helper_wait, &wait);
	do {
		struct lazy_de_info *info = NULL;

		current->state = TASK_INTERRUPTIBLE;

		spin_lock(&fetching_lock);
		if (!list_empty(&sbi->to_helper)) {

			info = list_entry(sbi->to_helper.next,
					struct lazy_de_info, helper_list);
			list_move(&info->helper_list,
				  &sbi->requests_in_progress);
		}
		spin_unlock(&fetching_lock);

		if (info) {
			struct dentry *dentry = info->dentry;

			err = send_to_helper(buffer, count, dentry);

			if (err < 0) {
				/* Failed... error */
				spin_lock(&fetching_lock);
				if (list_empty(&info->helper_list))
					BUG();
				list_del_init(&info->helper_list);
				spin_unlock(&fetching_lock);
				wake_up_interruptible(&lazy_wait);
			}
			dput(dentry);
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
        remove_wait_queue(&lazy_wait, &wait);

	return err;
}

/*			File operations				*/

static ssize_t
lazyfs_file_read(struct file *file, char *buffer, size_t count, loff_t *off)
{
	struct file *host_file = file->private_data;

	if (!host_file)
		BUG();

	if (host_file->f_op && host_file->f_op->read)
		return host_file->f_op->read(host_file, buffer, count, off);

	return -EINVAL;
}

static int
lazyfs_file_mmap(struct file *file, struct vm_area_struct *vm)
{
	struct file *host_file = file->private_data;
	struct inode *inode, *host_inode;
	int err;

	if (!host_file->f_op || !host_file->f_op->mmap)
		return -ENODEV;

	inode = file->f_dentry->d_inode;
	host_inode = host_file->f_dentry->d_inode;

	if (inode->i_mapping != &inode->i_data &&
	    inode->i_mapping != host_inode->i_mapping) {
		/* We already forwarded the host mapping, but it's changed.
		 * Can this happen? Coda seems to think so...
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
	if (inode->i_mapping == &inode->i_data)
		inode->i_mapping = host_inode->i_mapping;

	return 0;
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

static int
lazyfs_file_open(struct inode *inode, struct file *file)
{
	struct dentry *dentry = file->f_dentry;
	struct super_block *sb = inode->i_sb;
	struct lazy_sb_info *sbi = sb->u.generic_sbp;
	struct dentry *host_dentry;
	struct file *host_file;

	host_dentry = get_host_dentry(dentry, 1);
	if (IS_ERR(host_dentry))
		return PTR_ERR(host_dentry);

	/* Open the host file */
	{
		struct vfsmount *mnt = mntget(sbi->host_file->f_vfsmnt);
		host_file = dentry_open(host_dentry, mnt, file->f_flags);
		host_dentry = NULL;
		/* (mnt and dentry freed by here) */
	}

	if (IS_ERR(host_file))
		return PTR_ERR(host_file);
	inc("file->host_file");

	file->private_data = host_file;

	return 0;
}

static int
lazyfs_file_release(struct inode *inode, struct file *file)
{
	if (file->private_data) {
		fput((struct file *) file->private_data);
		dec("file->host_file");
	}
	else
		printk("WARNING: no private_data!\n");

	return 0;
}

/*			Classes					*/

static struct super_operations lazyfs_ops = {
	statfs:		lazyfs_statfs,
	put_super:	lazyfs_put_super,
	put_inode:	lazyfs_put_inode,
};

static struct inode_operations lazyfs_dir_inode_operations = {
	lookup:		lazyfs_lookup,
};

static struct file_operations lazyfs_helper_operations = {
	open:		lazyfs_helper_open,
	read:		lazyfs_helper_read,
	release:	lazyfs_helper_release,
};

static struct file_operations lazyfs_handle_operations = {
	read:		lazyfs_handle_read,
	release:	lazyfs_handle_release,
};

static struct inode_operations lazyfs_link_operations = {
	readlink:	lazyfs_link_readlink,
};

static struct file_operations lazyfs_file_operations = {
	open:		lazyfs_file_open,
	read:		lazyfs_file_read,
	mmap:		lazyfs_file_mmap,
	release:	lazyfs_file_release,
};

static struct file_operations lazyfs_dir_operations = {
	read:		generic_read_dir,
	readdir:	lazyfs_readdir,
	open:		lazyfs_dir_open,
};

static struct dentry_operations lazyfs_dentry_ops = {
	d_release:	lazyfs_release_dentry,
};

static DECLARE_FSTYPE(lazyfs_fs_type, "lazyfs", lazyfs_read_super, FS_LITTER);

static int __init init_lazyfs_fs(void)
{
	return register_filesystem(&lazyfs_fs_type);
}

static void __exit exit_lazyfs_fs(void)
{
	unregister_filesystem(&lazyfs_fs_type);
}

EXPORT_NO_SYMBOLS;

module_init(init_lazyfs_fs)
module_exit(exit_lazyfs_fs)
MODULE_LICENSE("GPL");
