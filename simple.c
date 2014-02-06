/*
 * A Simple Filesystem for the Linux Kernel.
 *
 * Initial author: Sankar P <sankar.curiosity@gmail.com>
 * Modified By : Pranay Kr. Srivastava
 * License: Creative Commons Zero License - http://creativecommons.org/publicdomain/zero/1.0/
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <linux/version.h>

#include "super.h"
#include "simple_fs.h"

#define INODE_CACHE_NAME "simplefs_inode_cache"

static int nr_mounts = 0;

/* A super block lock that must be used for any critical section operation on the sb,
 * such as: updating the free_blocks, inodes_count etc. */
static DEFINE_MUTEX(simplefs_sb_lock);
static DEFINE_MUTEX(simplefs_inodes_mgmt_lock);

/* FIXME: This can be moved to an in-memory structure of the simplefs_inode.
 * Because of the global nature of this lock, we cannot create
 * new children (without locking) in two different dirs at a time.
 * They will get sequentially created. If we move the lock
 * to a directory-specific way (by moving it inside inode), the
 * insertion of two children in two different directories can be
 * done in parallel */
static DEFINE_MUTEX(simplefs_directory_children_update_lock);

void simplefs_sb_sync(struct super_block *vsb)
{
	struct buffer_head *bh;
	struct simplefs_super_block *sb = SIMPLEFS_SB(vsb);

	bh = (struct buffer_head *)sb_bread(vsb,
					    SIMPLEFS_SUPERBLOCK_BLOCK_NUMBER);
	bh->b_data = (char *)sb;
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
}

void simplefs_inode_add(struct super_block *vsb, struct simplefs_inode *inode)
{
	struct simplefs_super_block *sb = SIMPLEFS_SB(vsb);
	struct buffer_head *bh;
	struct simplefs_inode *inode_iterator;

	if (mutex_lock_interruptible(&simplefs_inodes_mgmt_lock)) {
		printk(KERN_ERR "Failed to acquire mutex lock %s +%d\n",
		       __FILE__, __LINE__);
		return;
	}

	bh = (struct buffer_head *)sb_bread(vsb,
					    SIMPLEFS_INODESTORE_BLOCK_NUMBER);

	inode_iterator = (struct simplefs_inode *)bh->b_data;

	if (mutex_lock_interruptible(&simplefs_sb_lock)) {
		printk(KERN_ERR "Failed to acquire mutex lock %s +%d\n",
		       __FILE__, __LINE__);
		return;
	}

	/* Append the new inode in the end in the inode store */
	inode_iterator += sb->inodes_count;

	memcpy(inode_iterator, inode, sizeof(struct simplefs_inode));
	sb->inodes_count++;

	mark_buffer_dirty(bh);
	simplefs_sb_sync(vsb);
	brelse(bh);

	mutex_unlock(&simplefs_sb_lock);
	mutex_unlock(&simplefs_inodes_mgmt_lock);
}

/* This function returns a blocknumber which is free.
 * The block will be removed from the freeblock list.
 *
 * In an ideal, production-ready filesystem, we will not be dealing with blocks,
 * and instead we will be using extents 
 *
 * If for some reason, the file creation/deletion failed, the block number
 * will still be marked as non-free. You need fsck to fix this.*/
int simplefs_sb_get_a_freeblock(struct super_block *vsb, uint64_t * out)
{
	struct simplefs_super_block *sb = SIMPLEFS_SB(vsb);
	int i;
	int ret = 0;

	if (mutex_lock_interruptible(&simplefs_sb_lock)) {
		printk(KERN_ERR "Failed to acquire mutex lock %s +%d\n",
		       __FILE__, __LINE__);
		ret = -EINTR;
		goto end;
	}

	/* Loop until we find a free block. We start the loop from 3,
	 * as all prior blocks will always be in use */
	for (i = 3; i < SIMPLEFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED; i++) {
		if (sb->free_blocks & (1 << i)) {
			break;
		}
	}

	if (unlikely(i == SIMPLEFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED)) {
		printk(KERN_ERR "No more free blocks available");
		ret = -ENOSPC;
		goto end;
	}

	*out = i;

	/* Remove the identified block from the free list */
	sb->free_blocks &= ~(1 << i);

	simplefs_sb_sync(vsb);

end:
	mutex_unlock(&simplefs_sb_lock);
	return ret;
}

static int simplefs_sb_get_objects_count(struct super_block *vsb,
					 uint64_t * out)
{
	struct simplefs_super_block *sb = SIMPLEFS_SB(vsb);

	if (mutex_lock_interruptible(&simplefs_inodes_mgmt_lock)) {
		printk(KERN_ERR "Failed to acquire mutex lock %s +%d\n",
		       __FILE__, __LINE__);
		return -EINTR;
	}
	*out = sb->inodes_count;
	mutex_unlock(&simplefs_inodes_mgmt_lock);

	return 0;
}

static int simplefs_readdir(struct file *filp, void *dirent, filldir_t filldir)
{
	loff_t pos;
	struct inode *inode;
	struct super_block *sb;
	struct buffer_head *bh;
	struct simplefs_inode *sfs_inode;
	struct simplefs_dir_record *record;
	int i;

	pos = filp->f_pos;
	inode = filp->f_dentry->d_inode;
	sb = inode->i_sb;

	if (pos) {
		/* FIXME: We use a hack of reading pos to figure if we have filled in all data.
		 * We should probably fix this to work in a cursor based model and
		 * use the tokens correctly to not fill too many data in each cursor based call */
		return 0;
	}

	sfs_inode = SIMPLEFS_INODE(inode);

	if (unlikely(!S_ISDIR(sfs_inode->mode))) {
		printk(KERN_ERR
		       "inode [%llu][%lu] for fs object [%s] not a directory\n",
		       sfs_inode->inode_no, inode->i_ino,
		       filp->f_dentry->d_name.name);
		return -ENOTDIR;
	}

	bh = (struct buffer_head *)sb_bread(sb, sfs_inode->data_block_number);

	record = (struct simplefs_dir_record *)bh->b_data;
	for (i = 0; i < sfs_inode->dir_children_count; i++) {
		filldir(dirent, record->filename, SIMPLEFS_FILENAME_MAXLEN, pos,
			record->inode_no, DT_UNKNOWN);
		filp->f_pos += sizeof(struct simplefs_dir_record);
		pos += sizeof(struct simplefs_dir_record);
		record++;
	}
	brelse(bh);

	return 0;
}

/* This functions returns a simplefs_inode with the given inode_no
 * from the inode store, if it exists. */
struct simplefs_inode *simplefs_get_inode(struct super_block *sb,
					  uint64_t inode_no)
{
	struct simplefs_super_block *sfs_sb = SIMPLEFS_SB(sb);
	struct simplefs_inode *sfs_inode = NULL;

	int i;
	struct buffer_head *bh;

	/* The inode store can be read once and kept in memory permanently while mounting.
	 * But such a model will not be scalable in a filesystem with
	 * millions or billions of files (inodes) */
	bh = (struct buffer_head *)sb_bread(sb,
					    SIMPLEFS_INODESTORE_BLOCK_NUMBER);
	sfs_inode = (struct simplefs_inode *)bh->b_data;

#if 0
	if (mutex_lock_interruptible(&simplefs_inodes_mgmt_lock)) {
		printk(KERN_ERR "Failed to acquire mutex lock %s +%d\n",
		       __FILE__, __LINE__);
		return NULL;
	}
#endif
	for (i = 0; i < sfs_sb->inodes_count; i++) {
		if (sfs_inode->inode_no == inode_no) {
			/* FIXME: bh->b_data is probably leaking */
			return sfs_inode;
		}
		sfs_inode++;
	}
//      mutex_unlock(&simplefs_inodes_mgmt_lock);

	return NULL;
}

ssize_t simplefs_read(struct file * filp, char __user * buf, size_t len,
		      loff_t * ppos)
{
	/* Hack to make sure that we answer the read call only once and not loop infinitely.
	 * We need to implement support for filesize in inode to remove this hack */
	static int done = 0;

	/* After the commit dd37978c5 in the upstream linux kernel,
	 * we can use just filp->f_inode instead of the
	 * f->f_path.dentry->d_inode redirection */
	struct simplefs_inode *inode =
	    SIMPLEFS_INODE(filp->f_path.dentry->d_inode);
	struct buffer_head *bh;

	char *buffer;
	int nbytes;

	if (done) {
		done = 0;
		return 0;
	}

	if (*ppos >= inode->file_size) {
		/* Read request with offset beyond the filesize */
		return 0;
	}

	bh = (struct buffer_head *)sb_bread(filp->f_path.dentry->d_inode->i_sb,
					    inode->data_block_number);

	if (!bh) {
		printk(KERN_ERR "Reading the block number [%llu] failed.",
		       inode->data_block_number);
		return 0;
	}

	buffer = (char *)bh->b_data;
	nbytes = min((size_t) inode->file_size, len);

	if (copy_to_user(buf, buffer, nbytes)) {
		brelse(bh);
		printk(KERN_ERR
		       "Error copying file contents to the userspace buffer\n");
		return -EFAULT;
	}

	brelse(bh);

	*ppos += nbytes;

	done = 1;
	return nbytes;
}

/* FIXME: The write support is rudimentary. I have not figured out a way to do writes
 * from particular offsets (even though I have written some untested code for this below) efficiently. */
ssize_t simplefs_write(struct file * filp, const char __user * buf, size_t len,
		       loff_t * ppos)
{
	/* After the commit dd37978c5 in the upstream linux kernel,
	 * we can use just filp->f_inode instead of the
	 * f->f_path.dentry->d_inode redirection */
	struct inode *inode;
	struct simplefs_inode *sfs_inode;
	struct simplefs_inode *inode_iterator;
	struct buffer_head *bh;
	struct super_block *sb;

	char *buffer;
	int count;

	inode = filp->f_path.dentry->d_inode;
	sfs_inode = SIMPLEFS_INODE(inode);
	sb = inode->i_sb;

	if (*ppos + len >= SIMPLEFS_DEFAULT_BLOCK_SIZE) {
		printk(KERN_ERR "File size write will exceed a block");
		return -ENOSPC;
	}

	bh = (struct buffer_head *)sb_bread(filp->f_path.dentry->d_inode->i_sb,
					    sfs_inode->data_block_number);

	if (!bh) {
		printk(KERN_ERR "Reading the block number [%llu] failed.",
		       sfs_inode->data_block_number);
		return 0;
	}
	buffer = (char *)bh->b_data;

	/* Move the pointer until the required byte offset */
	buffer += *ppos;

	if (copy_from_user(buffer, buf, len)) {
		brelse(bh);
		printk(KERN_ERR
		       "Error copying file contents from the userspace buffer to the kernel space\n");
		return -EFAULT;
	}
	*ppos += len;

	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	/* Set new size
	 * sfs_inode->file_size = max(sfs_inode->file_size, *ppos);
	 *
	 * FIXME: What to do if someone writes only some parts in between ?
	 * The above code will also fail in case a file is overwritten with
	 * a shorter buffer */

	if (mutex_lock_interruptible(&simplefs_inodes_mgmt_lock)) {
		printk(KERN_ERR "Failed to acquire mutex lock %s +%d\n",
		       __FILE__, __LINE__);
		return -EINTR;
	}
	/* Save the modified inode */
	bh = (struct buffer_head *)sb_bread(sb,
					    SIMPLEFS_INODESTORE_BLOCK_NUMBER);

	sfs_inode->file_size = *ppos;

	inode_iterator = (struct simplefs_inode *)bh->b_data;

	if (mutex_lock_interruptible(&simplefs_sb_lock)) {
		printk(KERN_ERR "Failed to acquire mutex lock %s +%d\n",
		       __FILE__, __LINE__);
		return -EINTR;
	}

	count = 0;
	while (inode_iterator->inode_no != sfs_inode->inode_no
	       && count < SIMPLEFS_SB(sb)->inodes_count) {
		count++;
		inode_iterator++;
	}

	if (likely(count < SIMPLEFS_SB(sb)->inodes_count)) {
		inode_iterator->file_size = sfs_inode->file_size;
		printk(KERN_INFO
		       "The new filesize that is written is: [%llu] and len was: [%lu]\n",
		       sfs_inode->file_size, len);

		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
	} else {
		printk(KERN_ERR
		       "The new filesize could not be stored to the inode.");
		len = -EIO;
	}

	brelse(bh);

	mutex_unlock(&simplefs_sb_lock);
	mutex_unlock(&simplefs_inodes_mgmt_lock);

	return len;
}

const struct file_operations simplefs_file_operations = {
/*	.read = simplefs_read,
	.write = simplefs_write,
	*/
	.aio_read = generic_file_aio_read,
	.aio_write = generic_file_aio_write,
	.llseek = generic_file_llseek,
	.mmap = generic_file_mmap,
	.owner = THIS_MODULE
};

const struct file_operations simplefs_dir_operations = {
	.owner = THIS_MODULE,
	.readdir = simplefs_readdir,
	.read = generic_read_dir,
	.llseek = generic_file_llseek,

};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,2,0)
static int simplefs_create(struct inode *dir, struct dentry *dentry,
			   umode_t mode, bool excl);

static int simplefs_mkdir(struct inode *dir, struct dentry *dentry,
			  umode_t mode);

struct dentry *simplefs_lookup(struct inode *parent_inode,
			       struct dentry *child_dentry, unsigned int flags);
#else
static int simplefs_create(struct inode *dir, struct dentry *dentry,
			   int mode, struct nameidata *excl);

static int simplefs_mkdir(struct inode *dir, struct dentry *dentry,
			  int flags);

struct dentry *simplefs_lookup(struct inode *parent_inode,
			       struct dentry *child_dentry, struct nameidata *nameidata);
#endif

static struct inode_operations simplefs_inode_ops = {
	.create = simplefs_create,
	.lookup = simplefs_lookup,
	.mkdir = simplefs_mkdir,
};

static int simplefs_create_fs_object(struct inode *dir, struct dentry *dentry,
				     umode_t mode)
{
	struct inode *inode;
	struct simplefs_inode *sfs_inode;
	struct simplefs_inode *inode_iterator;
	struct super_block *sb;
	struct simplefs_dir_record *record;
	struct simplefs_inode *parent_dir_inode;
	struct buffer_head *bh;
	struct simplefs_dir_record *dir_contents_datablock;
	uint64_t count;
	int ret;

	if (mutex_lock_interruptible(&simplefs_directory_children_update_lock)) {
		printk(KERN_ERR "Failed to acquire mutex lock %s +%d\n",
		       __FILE__, __LINE__);
		return -EINTR;
	}
	sb = dir->i_sb;

	ret = simplefs_sb_get_objects_count(sb, &count);
	if (ret < 0) {
		mutex_unlock(&simplefs_directory_children_update_lock);
		return ret;
	}

	if (unlikely(count >= SIMPLEFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED)) {
		/* The above condition can be just == insted of the >= */
		printk(KERN_ERR
		       "Maximum number of objects supported by simplefs is already reached");
		mutex_unlock(&simplefs_directory_children_update_lock);
		return -ENOSPC;
	}

	if (!S_ISDIR(mode) && !S_ISREG(mode)) {
		printk(KERN_ERR
		       "Creation request but for neither a file nor a directory");
		mutex_unlock(&simplefs_directory_children_update_lock);
		return -EINVAL;
	}

	inode = new_inode(sb);
	if (!inode) {
		mutex_unlock(&simplefs_directory_children_update_lock);
		return -ENOMEM;
	}

	inode->i_sb = sb;
	inode->i_op = &simplefs_inode_ops;
	inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
	inode->i_ino = 10;

	/* Loop until we get an unique inode number */
	while (simplefs_get_inode(sb, inode->i_ino)) {
		/* inode inode->i_ino already exists */
		inode->i_ino++;
	}

	/* FIXME: This is leaking. We need to free all in-memory inodes sometime */
	sfs_inode = kmalloc(sizeof(struct simplefs_inode), GFP_KERNEL);
	sfs_inode->inode_no = inode->i_ino;
	inode->i_private = sfs_inode;
	sfs_inode->mode = mode;

	if (S_ISDIR(mode)) {
		printk(KERN_INFO "New directory creation request\n");
		sfs_inode->dir_children_count = 0;
		inode->i_fop = &simplefs_dir_operations;
	} else if (S_ISREG(mode)) {
		printk(KERN_INFO "New file creation request\n");
		sfs_inode->file_size = 0;
		inode->i_fop = &simplefs_file_operations;
	}

	/* First get a free block and update the free map,
	 * Then add inode to the inode store and update the sb inodes_count,
	 * Then update the parent directory's inode with the new child.
	 *
	 * The above ordering helps us to maintain fs consistency
	 * even in most crashes
	 */
	ret = simplefs_sb_get_a_freeblock(sb, &sfs_inode->data_block_number);
	if (ret < 0) {
		printk(KERN_ERR "simplefs could not get a freeblock");
		mutex_unlock(&simplefs_directory_children_update_lock);
		return ret;
	}

	simplefs_inode_add(sb, sfs_inode);

	record = kmalloc(sizeof(struct simplefs_dir_record), GFP_KERNEL);
	record->inode_no = sfs_inode->inode_no;
	strcpy(record->filename, dentry->d_name.name);

	parent_dir_inode = SIMPLEFS_INODE(dir);
	bh = sb_bread(sb, parent_dir_inode->data_block_number);
	dir_contents_datablock = (struct simplefs_dir_record *)bh->b_data;

	/* Navigate to the last record in the directory contents */
	dir_contents_datablock += parent_dir_inode->dir_children_count;

	memcpy(dir_contents_datablock, record,
	       sizeof(struct simplefs_dir_record));

	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	if (mutex_lock_interruptible(&simplefs_inodes_mgmt_lock)) {
		mutex_unlock(&simplefs_directory_children_update_lock);
		printk(KERN_ERR "Failed to acquire mutex lock %s +%d\n",
		       __FILE__, __LINE__);
		return -EINTR;
	}

	bh = (struct buffer_head *)sb_bread(sb,
					    SIMPLEFS_INODESTORE_BLOCK_NUMBER);

	inode_iterator = (struct simplefs_inode *)bh->b_data;

	if (mutex_lock_interruptible(&simplefs_sb_lock)) {
		printk(KERN_ERR "Failed to acquire mutex lock %s +%d\n",
		       __FILE__, __LINE__);
		return -EINTR;
	}

	count = 0;
	while (inode_iterator->inode_no != parent_dir_inode->inode_no
	       && count < SIMPLEFS_SB(sb)->inodes_count) {
		count++;
		inode_iterator++;
	}

	if (likely(inode_iterator->inode_no == parent_dir_inode->inode_no)) {
		parent_dir_inode->dir_children_count++;
		inode_iterator->dir_children_count =
		    parent_dir_inode->dir_children_count;
		/* Updated the parent inode's dir count to reflect the new child too */

		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
	} else {
		printk(KERN_ERR
		       "The updated childcount could not be stored to the dir inode.");
		/* TODO: Remove the newly created inode from the disk and in-memory inode store
		 * and also update the superblock, freemaps etc. to reflect the same.
		 * Basically, Undo all actions done during this create call */
	}

	brelse(bh);

	mutex_unlock(&simplefs_sb_lock);
	mutex_unlock(&simplefs_inodes_mgmt_lock);
	mutex_unlock(&simplefs_directory_children_update_lock);

	inode_init_owner(inode, dir, mode);
	d_add(dentry, inode);

	return 0;
}


#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,2,0)
static int simplefs_mkdir(struct inode *dir, struct dentry *dentry,
			  unmode_t mode)
{
	/* I believe this is a bug in the kernel, for some reason, the mkdir callback
	 * does not get the S_IFDIR flag set. Even ext2 sets is explicitly */
	return simplefs_create_fs_object(dir, dentry, S_IFDIR | mode);
}

static int simplefs_create(struct inode *dir, struct dentry *dentry,
			   umode_t mode, bool excl)
{
	return simplefs_create_fs_object(dir, dentry, mode);
}

struct dentry *simplefs_lookup(struct inode *parent_inode,
			       struct dentry *child_dentry, unsigned int flags)
{
	struct simplefs_inode *parent = SIMPLEFS_INODE(parent_inode);
	struct super_block *sb = parent_inode->i_sb;
	struct simplefs_dir_record *record;
	struct buffer_head *bh = NULL;
	int i;

	bh = (struct buffer_head *)sb_bread(sb, parent->data_block_number);
	record = (struct simplefs_dir_record *)bh->b_data;
	for (i = 0; i < parent->dir_children_count; i++) {
		if (!strcmp(record->filename, child_dentry->d_name.name)) {
			/* FIXME: There is a corner case where if an allocated inode,
			 * is not written to the inode store, but the inodes_count is
			 * incremented. Then if the random string on the disk matches
			 * with the filename that we are comparing above, then we
			 * will use an invalid unintialized inode */

			struct inode *inode;
			struct simplefs_inode *sfs_inode;

			/* FIXME: This simplefs_inode is leaking */
			sfs_inode = simplefs_get_inode(sb, record->inode_no);

			/* FIXME: This inode is leaking */
			inode = new_inode(sb);
			inode->i_ino = record->inode_no;
			inode_init_owner(inode, parent_inode, sfs_inode->mode);
			inode->i_sb = sb;
			inode->i_op = &simplefs_inode_ops;

			if (S_ISDIR(inode->i_mode))
				inode->i_fop = &simplefs_dir_operations;
			else if (S_ISREG(inode->i_mode))
				inode->i_fop = &simplefs_file_operations;
			else
				printk(KERN_ERR
				       "Unknown inode type. Neither a directory nor a file");

			/* FIXME: We should store these times to disk and retrieve them */
			inode->i_atime = inode->i_mtime = inode->i_ctime =
			    CURRENT_TIME;

			inode->i_private = sfs_inode;

			d_add(child_dentry, inode);
			return NULL;
		}
		record++;
	}

	printk(KERN_ERR
	       "No inode found for the filename [%s]\n",
	       child_dentry->d_name.name);

	return NULL;
}
#else
static int simplefs_create(struct inode *dir, struct dentry *dentry,
			   int mode, struct nameidata *excl)
{
	return simplefs_create_fs_object(dir, dentry, mode);
}

static int simplefs_mkdir(struct inode *dir, struct dentry *dentry,
			  int mode)
{
	/* I believe this is a bug in the kernel, for some reason, the mkdir callback
	 * does not get the S_IFDIR flag set. Even ext2 sets is explicitly */
	return simplefs_create_fs_object(dir, dentry, S_IFDIR | mode);
}


static uint64_t simplefs_locate_inode
		(struct super_block *sb,size_t len,
		 const char *buffer, const char *name)
{
	struct simple_fs_sb_i *msblk = SIMPLEFS_SB(sb);
	size_t nr_inodes_per_block = le32_to_cpu(msblk->sb.block_size)/
					SIMPLEFS_INODE_SIZE;
	if(len < DIR_RECORD_BASE_SIZE)
		return (uint64_t)-1;
	while(len && len >= DIR_RECORD_BASE_SIZE) {
		struct simplefs_dir_record_i *dir_entry = 
			(struct simplefs_dir_record_i*)buffer;
		if(!strncmp(dir_entry->filename,name,strlen(name))) {
			return le64_to_cpu(dir_entry->inode_no);
		}
		len -= dir_record_len(dir_entry);
		buffer += dir_record_len(dir_entry);
	}
return 0;
}

struct dentry* simplefs_lookup(struct inode *parent,
					struct dentry *child,
					struct nameidata *nameidata)
{
	struct simple_fs_inode_i *minode = SIMPLEFS_INODE(parent);
	struct simple_fs_sb_i *msblk = SIMPLEFS_SB(inode->i_sb);
	struct dentry *dentry = NULL;
	struct page *page = NULL;
	pgoff_t pg_index = 0;

	/*
	 * Lookup should be done in a more faster way, currently the
	 * only way is to linearly search for the directory entries.
	 * However we can't do that optimization over whole directory
	 * since directories can be huge and we don't want to alter
	 * other blocks of directory.
	 * Will think it over what can be done.
	 * */
	uint64_t dir_count = le64_to_cpu(minode->inode.dir_children_count);
	uint64_t inode_no = (uint64_t)-1;
	mutex_lock(&parent->i_mutex);	
		do {
			if(!dir_count)
				break;
			page = read_mapping_page(parent->i_mapping,
							pg_index,NULL);
			if(IS_ERR(page))
				break;
			kmap(page);
			inode_no = 
				simplefs_locate_inode(sb,PAGE_SIZE,
						page_address(page),
						child->d_name);
			if (inode_no > 0 ) {
				struct simplefs_inode *core_inode 
					= simplefs_read_inode(inode_no,inode->i_sb);
				if (!core_inode) {
					kunmap(page);
					break;
				}
				struct inode *new_inode = new_inode(inode_no);
				struct simple_fs_inode_i *new_sfs_inode = SIMPLEFS_INODE(new_sfs_inode);
				memcpy(&new_sfs_inode->inode,core_inode,SIMPLEFS_INODE_SIZE);
				if (!new_inode) {
					kunmap(page);
					break;
				}
				new_inode->i_mode = (umode_t)le64_to_cpu(new_sfs_inode->inode.mode);
				new_inode->i_atime = CURRENT_TIME;
				new_inode->i_ctime = ns_to_timespec(le64_to_cpu(new_sfs_inode->inode.ctime));
				new_inode->i_mtime = ns_to_timespec(le64_to_cpu(new_sfs_inode->inode.mtime));
				if(new_inode->i_mode & S_IFDIR)
					new_inode->i_op = &simplefs_dir_operations;
				else
					new_inode->i_op = &simplefs_file_operations;
				new_inode->i_mapping->a_ops = &simplefs_aops;
				d_add(dentry,new_inode);
			}
			kunmap(page);
		}while(1);
	mutex_unlock(&parent->i_mutex);
}

#if 0
struct dentry *simplefs_lookup(struct inode *parent_inode,
			       struct dentry *child_dentry, struct nameidata *nameidata)
{
	struct simplefs_inode *parent = SIMPLEFS_INODE(parent_inode);
	struct super_block *sb = parent_inode->i_sb;
	struct simplefs_dir_record *record;
	struct buffer_head *bh = NULL;
	int i;

	bh = (struct buffer_head *)sb_bread(sb, parent->data_block_number);
	record = (struct simplefs_dir_record *)bh->b_data;
	for (i = 0; i < parent->dir_children_count; i++) {
		if (!strcmp(record->filename, child_dentry->d_name.name)) {
			/* FIXME: There is a corner case where if an allocated inode,
			 * is not written to the inode store, but the inodes_count is
			 * incremented. Then if the random string on the disk matches
			 * with the filename that we are comparing above, then we
			 * will use an invalid unintialized inode */

			struct inode *inode;
			struct simplefs_inode *sfs_inode;

			/* FIXME: This simplefs_inode is leaking */
			sfs_inode = simplefs_get_inode(sb, record->inode_no);

			/* FIXME: This inode is leaking */
			inode = new_inode(sb);
			inode->i_ino = record->inode_no;
			inode_init_owner(inode, parent_inode, sfs_inode->mode);
			inode->i_sb = sb;
			inode->i_op = &simplefs_inode_ops;

			if (S_ISDIR(inode->i_mode))
				inode->i_fop = &simplefs_dir_operations;
			else if (S_ISREG(inode->i_mode))
				inode->i_fop = &simplefs_file_operations;
			else
				printk(KERN_ERR
				       "Unknown inode type. Neither a directory nor a file");

			/* FIXME: We should store these times to disk and retrieve them */
			inode->i_atime = inode->i_mtime = inode->i_ctime =
			    CURRENT_TIME;

			inode->i_private = sfs_inode;

			d_add(child_dentry, inode);
			return NULL;
		}
		record++;
	}

	printk(KERN_ERR
	       "No inode found for the filename [%s]\n",
	       child_dentry->d_name.name);

	return NULL;

}
#endif

#endif

struct simplefs_inode* simplefs_lookup_inode(const char *name,struct inode *parent)
{
	struct simple_fs_inode_i *minode = SIMPLEFS_INODE(parent);
	if(!S_ISDIR(minode->mode))
		return NULL;

}
struct simplefs_inode* simplefs_read_inode(uint64_t inode_no,struct super_block *sb) 
{
	struct simple_fs_sb_i *msblk = SIMPLEFS_SB(sb);
	int inodes_per_block = SIMPLEFS_INODE_SIZE/msblk->sb.block_size;
	struct simplefs_inode *inode = NULL;
try_again:
	 inode = 
		(struct simplefs_inode*)(msblk->inode_table[(inode_no - 1)/inodes_per_block]->b_data);
	if(!inode) {
		/*
		 * One more shot at reading the buffer head of inode table
		 * */
		msblk->inode_table[(inode_no - 1)/inodes_per_block ] = 
			sb_bread(sb,msblk->sb.inode_block_start + (inode_no - 1)/inodes_per_block);
		if (!msblk->inode_table[ (inode_no - 1)/inodes_per_block ])
			return NULL;
		goto try_again;
	}		
	return inode + ((inode_no - 1)%inodes_per_block);
}

/* This function, as the name implies, Makes the super_block valid and
 * fills filesystem specific information in the super block */
int simplefs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct inode *root_inode;
	struct buffer_head *bh;
	struct simple_fs_sb_i *msblk;
	struct simple_fs_inode_i *mroot_inode;
	struct simplefs_inode *dummy_inode;
	static char inode_cache_name[sizeof(INODE_CACHE_NAME) + 4 ];
	int j = 0;
	int blocks_per_buffer;

	bh = sb_bread(sb,SIMPLEFS_SUPERBLOCK_BLOCK_NUMBER);

	if (!bh)
		goto failed;
	msblk = kzalloc(sizeof(struct simple_fs_sb_i),GFP_KERNEL);

	if (!msblk)
		goto fail_bh;
	memcpy(&msblk->sb,bh->b_data,sizeof(struct simplefs_super_block));
	if( !(msblk->sb.char_version[0] & SIMPLEFS_ENDIANESS_LITTLE)) {
		/*
		 * Decides wether to work with big endian/
		 * little endianess.
		 * */
		super_to_cpu(le,&msblk->sb);
	}

	printk(KERN_INFO "The magic number obtained in disk is: [%llu]\n",
	       msblk->sb.magic);

	if (unlikely(msblk->sb.magic != SIMPLEFS_MAGIC)) {
		printk(KERN_ERR
		       "The filesystem that you try to mount is not of type simplefs. Magicnumber mismatch.");
		return -EPERM;
	}

	if (unlikely(msblk->sb.block_size != SIMPLEFS_DEFAULT_BLOCK_SIZE)) {
		printk(KERN_ERR
		       "simplefs seem to be formatted using a non-standard block size.");
		return -EPERM;
	}
	snprintf(inode_cache_name,sizeof(inode_cache_name) - 1
			,"%s%d",INODE_CACHE_NAME,++nr_mounts);
	msblk->inode_cachep = kmem_cache_create(inode_cache_name,
				sizeof(struct simple_fs_inode_i),0,
				SLAB_HWCACHE_ALIGN,NULL);
	if(!msblk->inode_cachep)
		goto fail_sb;

	printk(KERN_INFO
	       "simplefs filesystem of version [%u] formatted with a block size of [%u] detected in the device.\n",
	       msblk->sb.char_version[0], msblk->sb.block_size);

	/* A magic number that uniquely identifies our filesystem type */
	sb->s_magic = SIMPLEFS_MAGIC;

	/* For all practical purposes, we will be using this s_fs_info as the super block */
	sb->s_fs_info = msblk;
	sb->s_op = &simplefs_sops;
	blocks_per_buffer = /*msblk->sb.block_size >> PAGE_SHIFT*/1;
		
	msblk->inode_table =
	       	kcalloc( (msblk->sb.inode_bitmap_start
		       	- msblk->sb.inode_block_start + 1) / blocks_per_buffer + 1 
				,sizeof(void*),GFP_KERNEL);
	msblk->inode_bitmap = 
		kcalloc( (msblk->sb.block_bitmap_start
			- msblk->sb.inode_bitmap_start + 1) / blocks_per_buffer + 1 ,
				sizeof(void*),GFP_KERNEL);
	msblk->block_bitmap = 
		kcalloc( (msblk->sb.data_block_start 
			- msblk->sb.block_bitmap_start + 1) / blocks_per_buffer + 1,
				sizeof(void*),GFP_KERNEL);
	if (!msblk->inode_table || !msblk->inode_bitmap || !msblk->block_bitmap){
		goto fail_buffers;
	}
	for (j=0;
		j < (msblk->sb.inode_bitmap_start 
			- msblk->sb.inode_block_start+1)/blocks_per_buffer+1;j++) {
		/*
		 * Read in all the buffer heads for inode table
		 * 
		 * */
		msblk->inode_table[j] = sb_bread(sb,msblk->sb.inode_block_start + j);
		/*
		 * Don't do anything yet if we are not able to read the meta
		 * data for inode table. If it's already screwed up then you shouldn't
		 * be mounting it in the first place. So we'll do our best what we can
		 * to read data.
		 * */		
	}
	for (j=0;
		j < (msblk->sb.block_bitmap_start 
			- msblk->sb.inode_bitmap_start + 1) / blocks_per_buffer + 1;j++) {
		msblk->inode_bitmap[j] = sb_bread(sb,msblk->sb.inode_bitmap_start + j);
	}
	for(j=0;
		j < (msblk->sb.data_block_start
			- msblk->sb.block_bitmap_start + 1) / blocks_per_buffer + 1;j++) {
		msblk->block_bitmap[j] = sb_bread(sb,msblk->sb.block_bitmap_start);
	}

	root_inode = new_inode(sb);
	if (!root_inode) {
		goto fail_buffers;
	}
	mroot_inode = SIMPLEFS_INODE(root_inode);
	dummy_inode = simplefs_read_inode(SIMPLEFS_ROOTDIR_INODE_NUMBER,sb);
	
	if (!dummy_inode)
		goto fail_inode;

	memcpy(&mroot_inode->inode,dummy_inode,
			SIMPLEFS_INODE_SIZE);
	root_inode->i_ino = SIMPLEFS_ROOTDIR_INODE_NUMBER;
	inode_init_owner(root_inode, NULL, S_IFDIR);
	root_inode->i_sb = sb;
	root_inode->i_op = &simplefs_inode_ops;
	root_inode->i_fop = &simplefs_dir_operations;
	root_inode->i_atime = root_inode->i_mtime =  
		ns_to_timespec(mroot_inode->inode.m_time);
	root_inode->i_ctime = ns_to_timespec(mroot_inode->inode.c_time);

/*	root_inode->i_private =
	    simplefs_get_inode(sb, SIMPLEFS_ROOTDIR_INODE_NUMBER);
	    */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,2,0)
	sb->s_root = d_make_root(root_inode);
#else
	sb->s_root = d_alloc_root(root_inode);
#endif
	if (!sb->s_root)
		goto fail_inode;
	mutex_init(&msblk->sb_mutex);
	bforget(bh);
	return 0;
fail_inode:
	kmem_cache_free(msblk->inode_cachep,mroot_inode);
fail_buffers:
	kfree(msblk->inode_table);
	kfree(msblk->inode_bitmap);
	kfree(msblk->block_bitmap);
	kmem_cache_destroy(msblk->inode_cachep);
fail_sb:
	kfree(msblk);
fail_bh:
	bforget(bh);
failed:
	return -ENOMEM;

}

static struct dentry *simplefs_mount(struct file_system_type *fs_type,
				     int flags, const char *dev_name,
				     void *data)
{
	struct dentry *ret;

	ret = mount_bdev(fs_type, flags, dev_name, data, simplefs_fill_super);

	if (unlikely(IS_ERR(ret)))
		printk(KERN_ERR "Error mounting simplefs");
	else
		printk(KERN_INFO "simplefs is succesfully mounted on [%s]\n",
		       dev_name);
	return ret;
}

static void simplefs_kill_superblock(struct super_block *sb)
{
	struct simple_fs_sb_i *msblk = SIMPLEFS_SB(sb);
	simplefs_sync_metadata(sb);
	kfree(msblk->inode_table);
	kfree(msblk->inode_bitmap);
	kfree(msblk->block_bitmap);
	kmem_cache_destroy(msblk->inode_cachep);
	kfree(msblk);
	printk(KERN_INFO
	       "simplefs superblock is destroyed. Unmount succesful.\n");
	/* This is just a dummy function as of now. As our filesystem gets matured,
	 * we will do more meaningful operations here */
	return;
}

struct file_system_type simplefs_fs_type = {
	.owner = THIS_MODULE,
	.name = "simplefs",
	.mount = simplefs_mount,
	.kill_sb = simplefs_kill_superblock,
};

static int simplefs_init(void)
{
	int ret;

	ret = register_filesystem(&simplefs_fs_type);
	if (likely(ret == 0))
		printk(KERN_INFO "Sucessfully registered simplefs\n");
	else
		printk(KERN_ERR "Failed to register simplefs. Error:[%d]", ret);

	return ret;
}

static void simplefs_exit(void)
{
	int ret;

	ret = unregister_filesystem(&simplefs_fs_type);

	if (likely(ret == 0))
		printk(KERN_INFO "Sucessfully unregistered simplefs\n");
	else
		printk(KERN_ERR "Failed to unregister simplefs. Error:[%d]",
		       ret);
}

module_init(simplefs_init);
module_exit(simplefs_exit);

MODULE_LICENSE("CC0");
MODULE_AUTHOR("Sankar P");
MODULE_AUTHOR("Pranay Kr. Srivastava");
