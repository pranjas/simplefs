#ifndef SIMPLE_FS_H
#define SIMPLE_FS_H

#include <linux/buffer_head.h>
#include <linux/fs.h>
#include "super.h"

struct simple_fs_sb_i {
	struct simplefs_super_block sb;
	/*
	 * These 3 are array of pointers
	 * for meta-data. We need to figure
	 * how much to stuff in memory for larger bitmaps.
	 * These arrays are null terminated.
	 *
	 * Perhaps we could do that by breaking up the blocks
	 * into groups and then work on each group. Well
	 * that's another story we'll work on it later. right now
	 * let's try to get this shit going.
	 * */
	struct buffer_head **inode_table;
	struct buffer_head **block_bitmap;
	struct buffer_head **inode_bitmap;

	struct kmem_cache *inode_cachep;
};

struct simple_fs_inode_i {
	struct inode vfs_inode;
	struct simplefs_inode inode;
	/*
	 * Add more members as and when required.
	 * */
};
#endif
