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
	int inodes_per_block = SIMPLEFS_INODE_SIZE/msblk->sb.block_size;

	/*
	 * Find the inode table where we need to write this inode.
	 */
	struct buffer_head *inode_table = msblk->inode_table[(minode->inode.inode_no - 1)/inodes_per_block];
	struct simplefs_inode *disk_inode =
			(struct simplefs_inode*)(inode_table->b_data + (minode->inode.inode_no - 1) % inodes_per_block);
	
	minode->inode.m_time = timespec_to_ns(vfs_inode.m_time);
	minode->inode.m_time = cpu_to_le64(minode->inode.m_time);
	
	if(!(vfs_inode->i_mode & S_IFDIR)) {
		minode->inode.file_size = i_size_read(vfs_inode);
		minode->inode.file_size = cpu_to_le64(minode->inode.file_size);
	}
	
	memcpy(disk_inode,&minode->inode,sizeof(struct simplefs_inode));
	mark_buffer_dirty(inode_table);
	if(wbc->sync_mode == WB_SYNC_ALL) {
		SFSDBG("[SFS] Writeback control was to sync all in %s \n",__FUNCTION__);		
		sync_dirty_buffer(inode_table);
	}
	/*
	 * Perhaps we should sync dirty buffer here,
	 * but let's see how far we can go. The inode
	 * may actually not be written if we don't force
	 * sync and let the flush thread do it for us.
	 */
	SFSDBG("Not syncing in %s\n",__FUNCTION__);
}

static int allocate_data_blocks(struct inode *vfs_inode,int nr_blocks)
{
	struct simple_fs_sb_i *msblk = SIMPLEFS_SB(vfs_inode->i_sb);
	struct simple_fs_inode_i *minode = SIMPLEFS_INODE(vfs_inode);
	struct buffer_head *sb_buffer_bitmap = NULL;
	char *bitmap_buffer = NULL;
	int block_start = 0,block_no = 0;
	int bitmap_index = 0;
	int blocks_alloced = 0;
	int buffer_offset = 0; /*How many b_this_page has been done on a single bh*/
	int block_start_buffer_offset = 0;
	int block_start_bitmap_index = 0;
	if(!nr_blocks)
		return 0;

	mutex_lock(&msblk->sb_mutex);
new_bitmap_buffer:
		sb_buffer_bitmap = msblk->block_bitmap[bitmap_index];
		if(!sb_buffer_bitmap)
				goto out_failed;
allocate_block:
		bitmap_buffer = sb_buffer_bitmap->b_data;
		if( nr_blocks && 
				(block_no = 
					alloc_bmap(bitmap_buffer,sb_buffer_bitmap->b_size)) < 0) {
			sb_buffer_bitmap = sb_buffer_bitmap->b_this_page;
			if(sb_buffer_bitmap == mbslk->block_bitmap[bitmap_index]) {
				bitmap_index++; /*Move to next buffer head in the array*/
				buffer_offset = 0;
				goto new_bitmap_buffer;
			}
			else 
				buffer_offset++;
			goto allocate_block;
		}
		else if(block_no >= 0) {
			nr_blocks--;
			blocks_alloced++;
			if(!block_start) {
				block_start = block_no + 
						(((sb_buffer_bitmap->b_size * buffer_offset) 
										+ (PAGE_SIZE*bitmap_index)) << 3);
				block_start_buffer_offset = buffer_offset;
				block_start_bitmap_index = bitmap_index;
			}
			if(buffer_uptodate(sb_buffer_bitmap))
				mark_buffer_dirty(sb_buffer_bitmap);
			block_no = -1;
			if(nr_blocks)
				goto allocate_block;
		}
	
	mutex_unlock(&msblk->sb_mutex);
	return block_start; /*Return starting block number of the allocated blocks*/
out_failed:
	if(blocks_alloced) {
		/*
		 * Get the starting buffer head from where allocations
		 * were started.
		 */
		sb_buffer_bitmap = msblk->block_bitmap[block_start_bitmap_index];
		/*
		 * Get the starting block number relative to the buffer head.
		 */
		block_no = block_start - ((
				(sb_buffer_bitmap->b_size * block_start_buffer_offset)
					+(PAGE_SIZE * block_start_bitmap_index))) << 3;		
		/*
		 * Move to the correct buffer head within the page.
		 */
		while(block_start_buffer_offset) {
			sb_buffer_bitmap = sb_buffer_bitmap->b_this_page;
			block_start_buffer_offset--;
		}
		
		bitmap_buffer = sb_buffer_bitmap->b_data;
		while(blocks_alloced) {
			if(free_bmap(bitmap_buffer,sb_buffer_bitmap->b_size,block_no++)){
					blocks_alloced--;
					if(buffer_uptodate(sb_buffer_bitmap))
						mark_buffer_dirty(sb_buffer_bitmap);
			}
			/*
			 * There was no freeing of block because this block_no didn't
			 * belonged the starting buffer head. We need to move the
			 * buffer head to new buffer or perhaps we might need to
			 * get a new msblk->block_bitmap[j].
			 */
			else {
				sb_buffer_bitmap = sb_buffer_bitmap->b_this_page;
				if( sb_buffer_bitmap == msblk->block_bitmap[block_start_bitmap_index] )
					sb_buffer_bitmap = msblk->block_bitmap[++block_start_bitmap_index];
				block_no = 0; /*This is relative to the buffer head*/
				bitmap_buffer = sb_buffer_bitmap->b_data;
			}
		}
	}
	mutex_unlock(msblk->sb_mutex);
	return 0;
}

/*
 * This one is the heart and soul. Most of the stuff is taken care of
 * by fslib. All we need to do is write this one here and fill up the
 * buffer head with the information it wants. 
 *
 * This buffer head is actually a local variable, see mpage_readpages
 * so don't do anything unusual with this specially don't search for it
 * in the buffer head list for block device you won't find it however
 * you'll create it and then it won't be good. This buffer has been
 * attached with the page so we need not worry about that.
 *
 * What you do here is you create mapping of the iblock, its actually
 * a block in file. So based on the inode you'll map it to get the
 * actual file system block number to read.
 *
 * So you just churn up some blocks if required since same routine
 * would be called for writing data. For reads i guess this would be
 * pretty simple however in any case we need to use same stuff for 
 * read/write so be careful.
 *
 * Not sure about the return value, Perhaps the number of mapped blocks 
 * however mpage_readpages then sends it to "confused". Arrhh.....
 */

static int get_simplefs_block(struct inode *vfs_inode, sector_t iblock,
								 struct buffer_head *bh_result, int create)
{
	struct simple_fs_sb_i *msblk = SIMPLEFS_SB(vfs_inode->i_sb);
	struct simple_fs_inode_i *minode = SIMPLEFS_INODE(vfs_inode);
	uint64_t file_size = i_size_read(vfs_inode);
	int blocks_alloced = file_size/msblk->sb.block_size +
						(file_size%msblk->sb.block_size?1:0);
	int start_block = -1;
	if(create) {
		 /* Holes not yet supported*/
		 if(blocks_alloced < iblock) {
			/*
		 	 * Allocate blocks for this inode.
			 */
			if( (start_block = allocate_data_block(vfs_inode,iblock - blocks_alloced)) > 0) {
				/*
				 * We've got exactly iblock - blocks_alloced newer blocks
				 */	
			}
		}
	}
	/*
	 * Find the mapping but don't create it.
	 */

}
struct address_space_operations simplefs_aops ={
	.readpage 	= simplefs_read_page;
	.readpages 	= simplefs_read_pages;
	.writepage  = simplefs_write_page;
	.writepages = simplefs_write_pages;
	.write_begin = simplefs_write_begin;
	.write_end = simeplfs_write_end;
};

struct super_operations simplefs_sops= {
	.alloc_inode = simplefs_alloc_inode,
	.destroy_inode = simplefs_destroy_inode,
	.put_super = simplefs_put_super,
	.write_inode = simplefs_write_inode,
};
