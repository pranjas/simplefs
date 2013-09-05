#include <linux/fs.h>
#include "super.h"

static struct inode* simplefs_alloc_inode(struct super_block *sb) 
{
	struct simple_fs_sb_i *msblk = SIMPLEFS_SB(sb);
	struct simple_fs_inode_i *inode = 
			kmem_cache_alloc(msblk->inode_cachep,GFP_KERNEL);
	if(!inode)
		return NULL;
	return &inode->vfs_inode;
}

static void simplefs_destroy_inode(struct inode *vfs_inode) 
{
	struct simple_fs_inode_i *inode = SIMPLEFS_INODE(vfs_inode);
	struct simple_fs_sb_i *sb = SIMPLEFS_SB(vfs_inode->i_sb);	
	kmem_cache_free(sb->inode_cachep,inode);
}

static void simplefs_put_super(struct super_block *sb) 
{
	struct simple_fs_sb_i *msblk = SIMPLEFS_SB(sb);
	if(msblk->inode_cachep)
		kmem_cache_destroy(msblk->inode_cachep);	
	kfree(msblk);
	sb->s_private = NULL;
}

static int simplefs_write_inode(struct inode *vfs_inode, struct writeback_control *wbc) 
{
	/*
	 * We just need to write the inode here not it's pages.
	 */
	struct simple_fs_inode_i *minode = SIMPLEFS_INODE(vfs_inode);
	struct simple_fs_sb_i *msblk = SIMPLEFS_SB(vfs_inode->i_sb);
	int inodes_per_block = SIMPLEFS_INODE_SIZE

	/*
	 * Find the inode table where we need to write this inode.
	 */
	 minode->inode.inode_no


}
struct super_operations simplefs_sops= {
	.alloc_inode = simplefs_alloc_inode,
	.destroy_inode = simplefs_destroy_inode,
	.put_super = simplefs_put_super,
	.write_inode = simplefs_write_inode,
};
