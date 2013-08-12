#include <linux/fs.h>
#include "simple.h"
#include "simple_fs.h"
static inline struct simple_fs_sb_i *SIMPLEFS_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

static inline struct simple_fs_inode_i *SIMPLEFS_INODE(struct inode *inode)
{
	return container_of(inode,struct simple_fs_inode_i,vfs_inode);
}
extern struct super_operations simplefs_sops;
/*
 * This one syncs all the dirty buffer heads
 * which are being used for meta-data.
 */
extern void simplefs_sync_metadata(struct super_block *sb); 
