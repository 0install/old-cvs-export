/* Here are some functions copied from Linux-2.4.20, for people with older
 * kernels.
 */

/**
 * list_move - delete from one list and add as another's head
 * @list: the entry to move
 * @head: the head that will precede our entry
 */
static inline void list_move(struct list_head *list, struct list_head *head)
{
        __list_del(list->prev, list->next);
        list_add(list, head);
}

static int dcache_dir_open(struct inode *inode, struct file *file)
{
	static struct qstr cursor_name = {len:1, name:"."};

	file->private_data = d_alloc(file->f_dentry, &cursor_name);

	return file->private_data ? 0 : -ENOMEM;
}

static int dcache_dir_close(struct inode *inode, struct file *file)
{
	dput(file->private_data);
	return 0;
}

static loff_t dcache_dir_lseek(struct file *file, loff_t offset, int origin)
{
	down(&file->f_dentry->d_inode->i_sem);
	switch (origin) {
		case 1:
			offset += file->f_pos;
		case 0:
			if (offset >= 0)
				break;
		default:
			up(&file->f_dentry->d_inode->i_sem);
			return -EINVAL;
	}
	if (offset != file->f_pos) {
		file->f_pos = offset;
		if (file->f_pos >= 2) {
			struct list_head *p;
			struct dentry *cursor = file->private_data;
			loff_t n = file->f_pos - 2;

			spin_lock(&dcache_lock);
			p = file->f_dentry->d_subdirs.next;
			while (n && p != &file->f_dentry->d_subdirs) {
				struct dentry *next;
				next = list_entry(p, struct dentry, d_child);
				if (!list_empty(&next->d_hash) && next->d_inode)
					n--;
				p = p->next;
			}
			list_del(&cursor->d_child);
			list_add_tail(&cursor->d_child, p);
			spin_unlock(&dcache_lock);
		}
	}
	up(&file->f_dentry->d_inode->i_sem);
	return offset;
}

static int dcache_dir_fsync(struct file * file, struct dentry *dentry, int datasync)
{
	return 0;
}

static struct file_operations dcache_dir_ops = {
	open:		dcache_dir_open,
	release:	dcache_dir_close,
	llseek:		dcache_dir_lseek,
	read:		generic_read_dir,
	readdir:	dcache_readdir,
	fsync:		dcache_dir_fsync,
};
