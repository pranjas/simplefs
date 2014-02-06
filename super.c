#include <linux/fs.h>
#include "super.h"


static void simplefs_sync_metadata_buffer(struct bufffer_head **bh_table)
{
	struct buffer_head **walker = *bh_table;
	while(walker) {	
		struct buffer_head *bh = *walker;
		do
		{
			if(!buffer_uptodate(bh))
				sync_dirty_buffer(bh);
			bh=bh->b_this_page;
		}while(bh->b_this_page != *walker);
		walker++;
	}
}
void simplefs_sync_metadata(struct super_block *sb)
{
	struct simple_fs_sb_i *msblk = SIMPLEFS_SB(sb);
	/*
	 * Start with inodes.
	 */
	simplefs_sync_metadata_buffer(msblk->inode_table);
	simplefs_sync_metadata_buffer(msblk->inode_bitmap);
	simeplfs_sync_metadata_buffer(msblk->block_bitmap);
}

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
	if (inode->indirect_block) {
		if(!buffer_uptodate(inode->indirect_block))
			sync_dirty_buffer(inode->indirect_block);
		bforget(inode->indirect_block);
	}
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
	simplefs_sync_metadata(sb);
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
					+(PAGE_SIZE * block_start_bitmap_index))) * 8;		
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
	simplefs_sync_metadata(sb);
	mutex_unlock(msblk->sb_mutex);
	return 0;
}


/*
 * This one is the heart and soul. Most of the stuff is taken care of
 * by libfs. All we need to do is write this one here and fill up the
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

static int simplefs_get_block(struct inode *vfs_inode, sector_t iblock,
				struct buffer_head *bh_result, int create)
{
	struct simple_fs_sb_i *msblk = SIMPLEFS_SB(vfs_inode->i_sb);
	struct simple_fs_inode_i *minode = SIMPLEFS_INODE(vfs_inode);
	uint64_t mapped_block = -1;
	
	if(iblock > msblk->sb.block_size/sizeof(uint64_t))
		goto fail_get_block;
	
	if(create) {
		/* Do we already have allocated the indirect block.
		 * If yes then all we need to do is check the block location
		 * for being 0 within that.
		 */
		if(iblock) {
allocate_indirect_block:
			if(minode->inode.indirect_block_number) {
				if(!minode->indirect_block) {
					minode->indirect_block =
						sb_bread(vfs_inode->i_sb,
							minode->inode.indirect_block_number);
					if(!minode->indirect_block)
						goto fail_get_block;
				}
				uint64_t *block_offset = 
					((uint64_t*)(minode->indirect_block->b_data) + (iblock-1));
				mapped_block = le64_to_cpu(*block_offset);
				if(!mapped_block) {
					mapped_block = allocate_data_blocks(vfs_inode,1);
					if (!mapped_block) {
						SFSDBG(KERN_INFO "Error allocating indirect data block %s %d\n"
							,__FUNCTION__,__LINE__);
						goto fail_get_block;
					}
				}
				*block_offset = cpu_to_le64(mapped_block);
				mark_buffer_dirty(minode->indirect_block);
				mapped_block = block_offset;
			}
			else { /*Allocate that indirect block and the block within*/
				minode->inode.indirect_block_number = allocate_data_blocks(vfs_inode,1);
				if(!minode->inode.indirect_block_number) {
					SFSDBG(KERN_INFO "Error allocating indirect block %s %d\n"
							,__FUNCTION__,__LINE__);
					goto fail_get_block;
				}
				else
					goto allocate_indirect_block;
			}
		}
		else { /*This is the first block for the file*/
			if( minode->inode.data_block_number ){
				mapped_block = le64_to_cpu(minode->inode.data_block_number);
			}
			else
			{
				minode->inode.data_block_number = allocate_data_blocks(vfs_inode,1);
				if(!minode->inode.data_block_number) {
					SFSDBG(KERN_INFO "Error allocating direct block %s %d\n"
							,__FUNCTION__,__LINE__);
					goto fail_get_block;
				}
				mapped_block = minode->inode.data_block_number;
				minode->inode.data_block_number = cpu_to_le64(
						minode->inode.data_block_number);
			}
		}
	}
	else {
	
		/*
		 * Find the mapping but don't create it.
		 */
		if(iblock) {
			if(minode->inode.indirect_block_number) {
				if(!minode->indirect_block) {
					minode->indirect_block =
						sb_bread(vfs_inode->i_sb,
							minode->inode.indirect_block_number);
					if(!minode->indirect_block)
						goto fail_get_block;
				}
				uint64_t *block_offset = 
					((uint64_t*)(minode->indirect_block->b_data) + (iblock-1));
				mapped_block = le64_to_cpu(*block_offset);
				if(!mapped_block)
					goto fail_get_block;
			}
			else
				goto fail_get_block;
		}
		else {
			if(!minode->inode.data_block_number)
				goto fail_get_block;
			mapped_block = le64_to_cpu(minode->inode.data_block_number);
		}
	}
	set_buffer_new(bh_result);
	map_bh(bh_result,vfs_inode->i_sb,mapped_block);
	return 0;
fail_get_block:
	return -EOF;
}

static int simplefs_read_pages(struct file *filp,struct address_space *mapping
					,struct list_head *pages,unsigned nr_pages)
{
	SFSDBG(KERN_INFO "Read pages started \n");
	dump_stack();
	return mpage_readpages(mapping,pages,nr_pages,simplefs_get_block);
}
static int simplefs_write_pages(struct address_space *mapping,
				struct writeback_control *wbc)
{
	SFSDBG(KERN_INFO "Write pages started \n");
	dump_stack();
	return mpage_writepages(mapping,wbc,simplefs_get_block);
}

static int simplefs_read_page(struct file *filp,struct page *page)
{
	SFSDBG(KERN_INFO "Read page started \n");
	dump_stack();
	return mpage_readpage(page,simplefs_get_block);
}

static int simplefs_write_page(struct page *page,struct writeback_control *wbc)
{
	SFSDBG(KERN_INFO "Write page started \n");
	dump_stack();
	return mpage_writepage(page,simplefs_get_block,wbc);
}

int simplefs_write_begin(struct file *, struct address_space *mapping,
			loff_t pos, unsigned len, unsigned flags,
			struct page **pagep, void **fsdata)
{
	SFSDBG(KERN_INFO "Write begin started \n");
	dump_stack();
	return block_write_begin(mapping,pos,
			len,flags,pagep,simplefs_get_block);
}

int simplefs_write_end(struct file *file, struct address_space *mapping,
                               loff_t pos, unsigned len, unsigned copied,
                                struct page *page, void *fsdata)
{
	SFSDBG(KERN_INFO "Write end started \n");
	dump_stack();
	return generic_write_end(file,mapping,pos,
			len,copied,page,fsdata);
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
