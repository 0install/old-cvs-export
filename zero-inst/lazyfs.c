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


	/* WARNING: This code is very new, buggy and rapidly changing.
	 * Also, I've only been doing kernel-level programming for a week
	 * so far. Locking is sadly lacking. SMP machines WILL break.
	 * DO NOT run it on anything important (suggest using User Mode
	 * Linux to test it)
	 */

/* See the 'Technical' file for details. */

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

static void show_refs(struct dentry *dentry, int indent);

static DECLARE_WAIT_QUEUE_HEAD(lazy_wait);

static DECLARE_WAIT_QUEUE_HEAD(helper_wait);

static LIST_HEAD(to_helper);	/* Protected by fetching_lock */

struct lazy_sb_info
{
	struct file *host_file;
	struct qstr dirlist_qname;
	struct dentry *helper_dentry;
	int have_helper;	/* Protected by fetching_lock */
	struct vfsmount *helper_mnt;
};

struct lazy_de_info
{
	struct dentry *dentry;	/* dentry->d_fsdata == self */
	int may_delete;		/* Used during mark-and-sweep (XXX:lock) */

	/* Only for directories... */
	struct dentry *host_dentry;
	struct dentry *list_dentry; /* The host/... file */

	/* If 1, add yourself to lazy_wait and sleep. You'll be woken
	 * up when the host inode is ready.
	 */
	int fetching;
	struct list_head to_helper;
};

spinlock_t fetching_lock = SPIN_LOCK_UNLOCKED;

static struct super_operations lazyfs_ops;
static struct file_operations lazyfs_dir_operations;
static struct inode_operations lazyfs_dir_inode_operations;
static struct dentry_operations lazyfs_dentry_ops;
static struct file_operations lazyfs_file_operations;
static struct file_operations lazyfs_helper_operations;
static struct file_operations lazyfs_handle_operations;

static int ensure_cached(struct dentry *dentry);

/* A list of dentries which are waiting for a host */
static LIST_HEAD(pending_helper);

static void
lazyfs_release_dentry(struct dentry *dentry)
{
	struct lazy_de_info *info = (struct lazy_de_info *) dentry->d_fsdata;
	struct dentry *host_dentry;
	
	if (dentry->d_inode)
		BUG();
	if (!info)
		BUG();
	if (!list_empty(&info->to_helper))
		BUG();

	//printk("Putting dentry for '%s'\n", dentry->d_name.name);

	host_dentry = info->host_dentry;

	if (host_dentry)
		dput(host_dentry);

	dentry->d_fsdata = NULL;
	kfree(info);
}

/* If parent_dentry is NULL, then this creates a new root dentry,
 * otherwise, refs the parent.
 */
static struct dentry *new_dentry(struct super_block *sb,
				 struct dentry *parent_dentry,
				 const char *leaf,
				 mode_t mode,
				 loff_t size,
				 time_t mtime)
{
	struct dentry *new = NULL;
	struct inode *inode = NULL;
	struct lazy_de_info *info;

	inode = new_inode(sb);
	if (!inode)
		goto err;

	inode->i_mode = mode | 0444;	/* Always give read */
	inode->i_nlink = 1;
	inode->i_uid = 0;
	inode->i_gid = 0;
	inode->i_size = size;
	inode->i_atime = 0;
	inode->i_ctime = inode->i_mtime = mtime;
	if (S_ISDIR(mode))
	{
		inode->i_op = &lazyfs_dir_inode_operations;
		inode->i_fop = &lazyfs_dir_operations;
	}
	else
		inode->i_fop = &lazyfs_file_operations;

	if (parent_dentry)
	{
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

	//printk("New dentry '%s' with inode %ld\n", leaf, inode->i_ino);

	info = kmalloc(sizeof(struct lazy_de_info), GFP_KERNEL);
	if (!info)
		goto err;
	new->d_op = &lazyfs_dentry_ops;
	new->d_fsdata = info;
	info->dentry = new;
	info->host_dentry = NULL;
	info->list_dentry = NULL;
	info->fetching = 0;
	info->may_delete = 0;
	INIT_LIST_HEAD(&info->to_helper);

	if (parent_dentry)
		d_add(new, inode);

	return new;
err:
	if (inode)
		iput(inode);
	if (new)
		dput(new);
	return NULL;
}

/* Back this virtual dentry up with a real one. Only for directories. */
static void set_host_dentry(struct dentry *dentry, struct dentry *host)
{
	struct lazy_de_info *info = (struct lazy_de_info *) dentry->d_fsdata;

	if (!info || info->host_dentry)
		BUG();

	if (!S_ISDIR(dentry->d_inode->i_mode))
		BUG();

	info->host_dentry = dget(host);

	if (!info->host_dentry)
		BUG();
}

static struct super_block *
lazyfs_read_super(struct super_block *sb, void *data, int silent)
{
	struct lazy_sb_info *sbi = NULL;
	struct lazy_mount_data *mount_data = (struct lazy_mount_data *) data;
	struct inode *inode = NULL;
	struct file *file = NULL;

	sb->u.generic_sbp = NULL;
	if (!mount_data)
	{
		printk("lazyfs_read_super: Bad mount data\n");
		return NULL;
	}
	if (mount_data->version != 1)
	{
		printk("lazyfs_read_super: Wrong version number\n");
		return NULL;
	}

	file = fget(mount_data->fd);
	if (file)
		inode = file->f_dentry->d_inode;
	if (!inode || !S_ISDIR(inode->i_mode))
	{
		printk("lazyfs_read_super: Bad file\n");
		goto err;
	}

	sb->s_blocksize = 1024;
	sb->s_blocksize_bits = 10;
	sb->s_magic = LAZYFS_MAGIC;
	sb->s_op = &lazyfs_ops;

	sbi = kmalloc(sizeof(struct lazy_sb_info), GFP_KERNEL);
	if(!sbi)
		goto err;

	sbi->host_file = file;
	file = NULL;
	sb->u.generic_sbp = sbi;

	sb->s_root = new_dentry(sb, NULL, "/", S_IFDIR | 0111, 0,
			sbi->host_file->f_dentry->d_inode->i_mtime);
	if (!sb->s_root)
		goto err;
	set_host_dentry(sb->s_root, sbi->host_file->f_dentry);

	sbi->helper_dentry = new_dentry(sb, sb->s_root, ".lazyfs-helper",
			0, 0, CURRENT_TIME);
	sbi->helper_dentry->d_inode->i_fop = &lazyfs_helper_operations;
	sbi->helper_dentry->d_inode->i_mode = S_IFREG | 0600;
	sbi->have_helper = 0;
	sbi->helper_mnt = NULL;

	/* Hash '...'. If fs provides its own hash, that will override
	 * this, but that should be OK.
	 */
	sbi->dirlist_qname.name = "...";
	sbi->dirlist_qname.len = 3;
	sbi->dirlist_qname.hash = full_name_hash(sbi->dirlist_qname.name,
						 sbi->dirlist_qname.len);

	return sb;
err:
	if (sbi)
		kfree(sbi);
	if (file)
		fput(file);
	if (sb->s_root)
		dput(sb->s_root);
	return NULL;
}

static struct dentry *get_host_dir_dentry(struct dentry *dentry,
					  struct dentry *host_dentry)
{
	struct dentry *list_dentry;
	struct dentry *old;
	struct lazy_de_info *info = (struct lazy_de_info *) dentry->d_fsdata;
	struct super_block *sb = dentry->d_inode->i_sb;
	struct lazy_sb_info *sbi = sb->u.generic_sbp;

	/* For directories, we also need to check that the '...' file is OK */
	down(&host_dentry->d_inode->i_sem);
	list_dentry = lookup_hash(&sbi->dirlist_qname, host_dentry);
	up(&host_dentry->d_inode->i_sem);

	if (IS_ERR(list_dentry))
		return list_dentry;
	if (!list_dentry->d_inode)
		goto fetch;
	if (!S_ISREG(list_dentry->d_inode->i_mode))
		goto fetch;

	/* '...' is valid */

	/* Cache host_dentry */
	spin_lock(&fetching_lock);
	old = info->host_dentry;
	info->host_dentry = dget(host_dentry);
	if (old)
		dput(old);
	spin_unlock(&fetching_lock);

	return list_dentry;
fetch:
	dput(list_dentry);
	return NULL;
}

/* Try to open parent_host/dentry.name. If it works, and it has the correct
 * type, and (for directories) has a '...' file, return it (or the '...').
 *
 * Returns NULL if there is no error, but the host does not exist yet, or
 * if it exists, but is the wrong type of object.
 */
static struct dentry *try_get_host_dentry(struct dentry *dentry,
					  struct dentry *parent_host)
{
	struct dentry *host_dentry;
	mode_t mode, host_mode;
	struct qstr name = dentry->d_name;
	struct super_block *sb = dentry->d_inode->i_sb;
	
	if (dentry == sb->s_root)
	{
		struct lazy_sb_info *sbi = sb->u.generic_sbp;
		host_dentry = dget(sbi->host_file->f_dentry);
	}
	else
	{
		down(&parent_host->d_inode->i_sem);
		host_dentry = lookup_hash(&name, parent_host);
		up(&parent_host->d_inode->i_sem);
	}

	if (IS_ERR(host_dentry))
		return host_dentry;

	if (!host_dentry)
		BUG();

	if (!host_dentry->d_inode)
		goto fetch;

	mode = dentry->d_inode->i_mode;
	host_mode = host_dentry->d_inode->i_mode;

	if (S_ISREG(mode))
	{
		if (S_ISREG(host_mode))
			return host_dentry;	/* File, OK */
		goto fetch;
	}
	else if (S_ISDIR(mode))
	{
		struct dentry *list_dentry;

		list_dentry = get_host_dir_dentry(dentry, host_dentry);
		dput(host_dentry);
		return list_dentry;
	}

	BUG();

fetch:
	/* host_dentry exists, but is negative or the wrong type */
	dput(host_dentry);
	return NULL;
}

/* Return the dentry for this host. It's parent directory must already have
 * one. If the host inode does not yet exist, it sleeps until the helper
 * creates it.
 * If 'dentry' is a directory, then the returned value will be the dentry
 * of the '...' file. The host_dentry for the directory itself is cached.
 */
static struct dentry *get_host_dentry(struct dentry *dentry)
{
	struct super_block *sb = dentry->d_inode->i_sb;
	struct lazy_sb_info *sbi = (struct lazy_sb_info *) sb->u.generic_sbp;
	struct dentry *parent;
	struct dentry *host_dentry;
	struct lazy_de_info *parent_info;
	struct dentry *parent_host;
	struct lazy_de_info *info = (struct lazy_de_info *) dentry->d_fsdata;
	int first_try = 1;
	DECLARE_WAITQUEUE(wait, current);

	if (!info)
		BUG();

	//printk("get_host_dentry: %ld\n", dentry->d_inode->i_ino);

	parent = dentry->d_parent;
	parent_info = (struct lazy_de_info *) parent->d_fsdata;
	if (!parent_info)
		BUG();
	/* (parent_host can't be NULL, but it can change under us; dget) */
	parent_host = dget(parent_info->host_dentry);
	if (!parent_host)
		BUG();

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

		if (!first_try)
		{
			host_dentry = ERR_PTR(-EIO);
			goto out;
		}
		first_try = 0;

		/* Start a fetch, if there isn't one already */
		spin_lock(&fetching_lock);
		had_helper = sbi->have_helper;
		if (!info->fetching && sbi->have_helper)
		{
			info->fetching = 1;
			dget(dentry);
			list_add(&info->to_helper, &to_helper);
			start_fetching = 1;
		}
		spin_unlock(&fetching_lock);
		host_dentry = ERR_PTR(-EIO);
		if (!had_helper)
			goto out;
		if (start_fetching)
			wake_up_interruptible(&helper_wait);
		/* else someone else is already fetching it */

#if 0
		if (file->f_flags & O_NONBLOCK) {
                        retval = -EAGAIN;
                        goto out;
                }
#endif
		while (1)
		{
			current->state = TASK_INTERRUPTIBLE;
			spin_lock(&fetching_lock);
			if (!info->fetching)
			{
				spin_unlock(&fetching_lock);
				break;
			}
			spin_unlock(&fetching_lock);

			if (signal_pending(current)) {
				host_dentry = ERR_PTR(-ERESTARTSYS);
				goto out;
			}
			
			//printk("get_host_dentry: sleeping...\n");
			schedule();
			//printk("get_host_dentry: try again...\n");
		}
		//printk("get_host_dentry: it's us!\n");
	} while (1);

out:
	dput(parent_host);
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

static void mark_children_may_delete(struct dentry *dentry)
{
	struct super_block *sb = dentry->d_inode->i_sb;
	struct lazy_sb_info *sbi = (struct lazy_sb_info *) sb->u.generic_sbp;
	struct list_head *head, *next;

	spin_lock(&dcache_lock);
	head = &dentry->d_subdirs;
	next = head->next;

	while (next != head) {
		struct dentry *child = list_entry(next, struct dentry, d_child);
		struct lazy_de_info *info = (struct lazy_de_info *)
						child->d_fsdata;
		next = next->next;

		if (d_unhashed(child)||!child->d_inode)
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
		if (d_unhashed(dentry)||!dentry->d_inode)
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

/* Removes dentry from the tree, and any child nodes too */
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
		struct lazy_de_info *info = (struct lazy_de_info *)
						child->d_fsdata;
		next = next->next;

		if (d_unhashed(child)||!child->d_inode)
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

/* The file list for a directory has changed.
 * Update it by parsing the new list of contents read from '...'.
 */
static int
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

	lock_kernel();

	mark_children_may_delete(dir);

	while (listing < end)
	{
		struct dentry *existing;
		mode_t mode = 0444;
		struct qstr name;
		off_t size;
		time_t time;

		switch (*(listing++))
		{
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
		if (listing == end + 1)
			goto bad_list;	/* Last line not terminated */

		name.hash = full_name_hash(name.name, name.len);
		existing = d_lookup(dir, &name);
		if (existing && existing->d_inode)
		{
			struct inode *i = existing->d_inode;
			if (i->i_mode != mode || i->i_size != size ||
			    i->i_mtime != time)
			{
				remove_dentry(existing);
				new_dentry(sb, dir, name.name,
					   mode, size, time);
			}
			else
			{
				struct lazy_de_info *info =
					(struct lazy_de_info *)
						existing->d_fsdata;
				info->may_delete = 0;
			}
			dput(existing);
		}
		else
			new_dentry(sb, dir, name.name, mode, size, time);
	}

	sweep_marked_children(dir);

	unlock_kernel();

	if (listing != end)
		BUG();

	show_refs(dir->d_inode->i_sb->s_root, 0);
	
	return 0;

bad_list:
	unlock_kernel();

	printk("lazyfs: '...' file is invalid\n");
	return -EIO;
}

/* Make sure the dcache relects the contents of '...'. If '...' is
 * missing, try to fetch it now.
 */
static int ensure_cached(struct dentry *dentry)
{
	struct lazy_de_info *info = (struct lazy_de_info *) dentry->d_fsdata;
	struct super_block *sb = dentry->d_inode->i_sb;
	struct lazy_sb_info *sbi = (struct lazy_sb_info *) sb->u.generic_sbp;
	struct dentry *list_dentry = NULL;
	struct file *ddd_file;
	int err;

	//printk("ensure_cached: %ld\n", dentry->d_inode->i_ino);

	if (!S_ISDIR(dentry->d_inode->i_mode))
		BUG();
	if (!info)
		BUG();
	if (!sbi || !sbi->host_file)
		BUG();

	list_dentry = get_host_dentry(dentry);
	if (IS_ERR(list_dentry))
		return PTR_ERR(list_dentry);

	if (list_dentry == info->list_dentry)
	{
		/* Already cached... do nothing */
		//printk("Already cached\n");
		dput(list_dentry);
		return 0;
	}

	/* Switch to the new version */
	if (info->list_dentry)
		dput(info->list_dentry);
	info->list_dentry = dget(list_dentry);
	
	/* Open the '...' file */
	{
		struct vfsmount *mnt = mntget(sbi->host_file->f_vfsmnt);
		/* Open for reading */
		ddd_file = dentry_open(list_dentry, mnt, 1);
		list_dentry = NULL;
		mnt = NULL;
		/* (mnt and dentry freed by here) */
	}

	if (IS_ERR(ddd_file))
		return PTR_ERR(ddd_file);
	else
	{
		off_t offset = 0;
		off_t size;
		char *listing;

		/* Load the '...' file into memory and process it */

		size = ddd_file->f_dentry->d_inode->i_size;
		if (size > LAZYFS_MAX_LISTING_SIZE)
		{
			printk("lazyfs: '...' file too big\n");
			fput(ddd_file);
			return -E2BIG;
		}

		listing = kmalloc(size + 1, GFP_KERNEL);
		if (!listing)
		{
			fput(ddd_file);
			return -ENOMEM;
		}
		listing[size] = '\0';

		while (offset < size)
		{
			int got;
			got = kernel_read(ddd_file, offset, listing + offset,
					size - offset);
			if (got != size - offset)
			{
				printk("lazyfs: error reading '...'\n");
				fput(ddd_file);
				kfree(listing);
				return got < 0 ? got : -EIO;
			}
			offset += got;
		}
		fput(ddd_file);

		err = add_dentries_from_list(dentry, listing, size);

		kfree(listing);
	}

	return err;
}

static int
lazyfs_dir_open(struct inode *inode, struct file *file)
{
	//printk("lazyfs_opendir: %ld\n", file->f_dentry->d_inode->i_ino);

	/* Make sure the dcache contains the correct structure for
	 * this directory.
	 * readdir can fetch everything from the dcache...
	 */
	return ensure_cached(file->f_dentry);
}

static void
lazyfs_put_super(struct super_block *sb)
{
	struct lazy_sb_info *sbi = (struct lazy_sb_info *) sb->u.generic_sbp;

	if (sbi)
	{
		struct file *file = sbi->host_file;
		dput(sbi->helper_dentry);
		sbi->helper_dentry = NULL;
		sbi->host_file = NULL;
		fput(file);
		sb->u.generic_sbp = NULL;
		kfree(sbi);
	}
	else
		BUG();
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
	else
	{
		err = filldir(dirent, ".", 1, 0, dir->d_inode->i_ino, DT_DIR);
		if (err)
			return count ? count : err;
		file->f_pos++;
		count++;
	}

	if (skip)
		skip--;
	else
	{
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
		next = next->next;

		if (d_unhashed(child)||!child->d_inode)
			continue;

		if (skip)
		{
			skip--;
			continue;
		}

		//printk("Got child '%s'\n", child->d_name.name);
		file->f_pos++;
		err = filldir(dirent, child->d_name.name,
				      child->d_name.len,
			      file->f_pos,
			      child->d_inode->i_ino,
			      0); /* XXX: type unused */
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

	//printk("lazyfs_lookup: %s : %s\n", dentry->d_parent->d_name.name,
			//dentry->d_name.name);

	if (dir == dir->i_sb->s_root->d_inode &&
		strcmp(dentry->d_name.name, ".lazyfs-helper") == 0)
	{
		struct lazy_sb_info *sbi = (struct lazy_sb_info *)
						dir->i_sb->u.generic_sbp;
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
	struct lazy_de_info *info = (struct lazy_de_info *) dentry->d_fsdata;

	if (!info)
		BUG();

	printk("Helper finished handling '%s'\n", dentry->d_name.name);

	spin_lock(&fetching_lock);
	if (!info->fetching)
		BUG();
	info->fetching = 0;
	spin_unlock(&fetching_lock);

	wake_up_interruptible(&lazy_wait);
}

/* We create a new file object and pass it to userspace. When closed, we
 * check to see if the host has been created. If we don't return an error
 * then lazyfs_handle_release will get called eventually for this dentry...
 */
static ssize_t
send_to_helper(char *buffer, size_t count, struct dentry *dentry)
{
	struct super_block *sb = dentry->d_inode->i_sb;
	struct lazy_sb_info *sbi = (struct lazy_sb_info *) sb->u.generic_sbp;
	struct file *file;
	char number[20];
	int len = dentry->d_name.len;
	int fd;

	printk("Sending '%s' to helper\n", dentry->d_name.name);

	file = get_empty_filp();
	if (!file)
		return -ENOMEM;
	fd = get_unused_fd();
	if (fd < 0)
	{
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

	copy_to_user(buffer, number, len + 1);

	return len + 1;
}

static int
lazyfs_helper_open(struct inode *inode, struct file *file)
{
	struct super_block *sb = inode->i_sb;
	struct lazy_sb_info *sbi = (struct lazy_sb_info *) sb->u.generic_sbp;
	int err = 0;

	if (!sbi)
		BUG();

	spin_lock(&fetching_lock);
	if (sbi->have_helper)
		err = -EBUSY;
	else
		sbi->have_helper = 1;
	spin_unlock(&fetching_lock);

	if (!err)
		sbi->helper_mnt = mntget(file->f_vfsmnt);

	return err;
}

static int
lazyfs_helper_release(struct inode *inode, struct file *file)
{
	struct super_block *sb = inode->i_sb;
	struct lazy_sb_info *sbi = (struct lazy_sb_info *) sb->u.generic_sbp;

	if (!sbi)
		BUG();

	mntput(sbi->helper_mnt);
	sbi->helper_mnt = NULL;

	spin_lock(&fetching_lock);
	while (!list_empty(&to_helper)) {
		struct lazy_de_info *info;
		info = list_entry(to_helper.next,
				struct lazy_de_info, to_helper);

		list_del_init(&info->to_helper);

		printk("Discarding '%s'\n", info->dentry->d_name.name);
		if (info->fetching != 1)
			BUG();
		info->fetching = 0;
	}

	if (sbi->have_helper != 1)
		BUG();
	sbi->have_helper = 0;

	spin_unlock(&fetching_lock);

	wake_up_interruptible(&lazy_wait);

	return 0;
}

static ssize_t
lazyfs_helper_read(struct file *file, char *buffer, size_t count, loff_t *off)
{
	int err = 0;
	DECLARE_WAITQUEUE(wait, current);

	if (count < 20)
		return -EINVAL;

	add_wait_queue(&helper_wait, &wait);
	do {
		struct lazy_de_info *info = NULL;

		current->state = TASK_INTERRUPTIBLE;

		spin_lock(&fetching_lock);
		if (!list_empty(&to_helper)) {

			info = list_entry(to_helper.next,
					struct lazy_de_info, to_helper);
			list_del_init(&info->to_helper);
		}
		spin_unlock(&fetching_lock);

		if (info)
		{
			struct dentry *dentry = info->dentry;

			//printk("Handle '%s'\n", dentry->d_name.name);
			err = send_to_helper(buffer, count, dentry);

			if (err < 0)
			{
				/* Failed... error */
				spin_lock(&fetching_lock);
				if (info->fetching != 1)
					BUG();
				info->fetching = 0;
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
	struct file *host_file = (struct file *) file->private_data;

	if (!host_file)
		BUG();

	if (host_file->f_op && host_file->f_op->read)
		return host_file->f_op->read(host_file, buffer, count, off);

	return -EINVAL;
}

static int
lazyfs_file_mmap(struct file *file, struct vm_area_struct *vm)
{
	struct file *host_file = (struct file *) file->private_data;
	struct inode *inode, *host_inode;
	int err;

	if (!host_file)
		BUG();

	if (!host_file->f_op || !host_file->f_op->mmap)
		return -ENODEV;

	inode = file->f_dentry->d_inode;
	host_inode = host_file->f_dentry->d_inode;

	if (inode->i_mapping != &inode->i_data &&
	    inode->i_mapping != host_inode->i_mapping)
	{
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
	{
		//printk("lazyfs_file_mmap: Forwarding mapping\n");
		inode->i_mapping = host_inode->i_mapping;
	}

	return 0;
}

static int
lazyfs_file_open(struct inode *inode, struct file *file)
{
	struct dentry *dentry = (struct dentry *) file->f_dentry;
	struct super_block *sb = inode->i_sb;
	struct lazy_sb_info *sbi = (struct lazy_sb_info *) sb->u.generic_sbp;
	struct dentry *host_dentry;
	struct file *host_file;

	host_dentry = get_host_dentry(dentry);
	if (IS_ERR(host_dentry))
		return PTR_ERR(host_dentry);

	if (!sbi || !sbi->host_file)
		BUG();

	/* Open the host file */
	{
		struct vfsmount *mnt = mntget(sbi->host_file->f_vfsmnt);
		host_file = dentry_open(host_dentry, mnt, file->f_flags);
		host_dentry = NULL;
		mnt = NULL;
		/* (mnt and dentry freed by here) */
	}

	if (IS_ERR(host_file))
		return PTR_ERR(host_file);

	file->private_data = host_file;

	return 0;
}

static int
lazyfs_file_release(struct inode *inode, struct file *file)
{
	if (file->private_data)
		fput((struct file *) file->private_data);
	else
		printk("WARNING: no private_data!\n");

	return 0;
}

/*			Classes					*/

static struct super_operations lazyfs_ops = {
	statfs:		lazyfs_statfs,
	put_super:	lazyfs_put_super,
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
	release:	lazyfs_handle_release,
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
