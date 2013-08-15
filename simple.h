#define SIMPLEFS_MAGIC 0x10032013
#define SIMPLEFS_DEFAULT_BLOCK_SIZE 4096
#define SIMPLEFS_FILENAME_MAXLEN 255

#define SIMPLEFS_ENDIANESS_BIG	0
#define SIMPLEFS_ENDIANESS_LITTLE 1

#ifndef __KERNEL__
#include <endian.h>
#define cpu_to_be(val,X)	htobe##X(val)
#define cpu_to_le(val,X)	htole##X(val)
#define be_to_cpu(val,X)	be##Xtoh(val)
#define le_to_cpu(val,X)	le##Xtoh(val)
#else
#define cpu_to_be(val,X)	cpu_to_be##X(val)
#define cpu_to_le(val,X)	cpu_to_le##X(val)
#define le_to_cpu(val,X)	le##X_to_cpu(val)
#define be_to_cpu(val,X)	be##X_to_cpu(val)
#endif

/* Hard-coded inode number for the root directory */
const int SIMPLEFS_ROOTDIR_INODE_NUMBER = 1;

/* The disk block where super block is stored */
const int SIMPLEFS_SUPERBLOCK_BLOCK_NUMBER = 0;

/* The disk block where the inodes are stored */
const int SIMPLEFS_INODESTORE_BLOCK_NUMBER = 1;

/* The disk block where the name+inode_number pairs of the
 * contents of the root directory are stored */
const int SIMPLEFS_ROOTDIR_DATABLOCK_NUMBER = 2;

/* The name+inode_number pair for each file in a directory.
 * This gets stored as the data for a directory */
struct simplefs_dir_record {
	uint64_t inode_no;
	uint8_t	 name_len;
	char filename[SIMPLEFS_FILENAME_MAXLEN];
};
#define DIR_RECORD_BASE_SIZE		(sizeof(struct simplefs_dir_record) - SIMPLEFS_FILENAME_MAXLEN)
#define dir_record_len(dir_record)	(DIR_RECORD_BASE_SIZE + (dir_record)->name_len)

struct simplefs_inode {
	uint64_t  mode;
	uint64_t inode_no;
	uint64_t data_block_number;

	union {
		uint64_t file_size;
		uint64_t dir_children_count;
	};
};

const int SIMPLEFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED = 64;
/* min (
		SIMPLEFS_DEFAULT_BLOCK_SIZE / sizeof(struct simplefs_inode),
		sizeof(uint64_t) //The free_blocks tracker in the sb 
 	); */

/* FIXME: Move the struct to its own file and not expose the members
 * Always access using the simplefs_sb_* functions and 
 * do not access the members directly 
 *
 * Version always stored as Little Endian.
 * */
struct simplefs_super_block {
	uint64_t magic;
	/* FIXME: This should be moved to the inode store and not part of the sb */
	uint64_t inodes_count;
	uint64_t free_blocks;
	uint64_t nr_blocks;
	uint64_t inode_block_start;
	uint64_t inode_bitmap_start;
	uint64_t block_bitmap_start;
	uint64_t data_block_start;
	uint32_t block_size;
	uint32_t version; /*The last bit on=LITTLE_ENDIAN, off=BIG_ENDIAN*/
	char padding[SIMPLEFS_DEFAULT_BLOCK_SIZE - (9 * sizeof(uint64_t))];
};

struct simplefs_super_block_inode_info {
	struct simplefs_super_block *sb_on_disk;
	struct buffer_head **bh; /* Allocate this on the fly*/
};

#define cpu_super_to(endianess,sb)\
	({\
		sb->magic = cpu_to_##endianess(sb->magic,64);\
		sb->block_size = cpu_to_##endianess(sb->block_size,32);\
		sb->inodes_count = cpu_to_##endianess(sb->inodes_count,64);\
		sb->free_blocks = cpu_to_##endianess(sb->free_blocks,64);\
	})

#define super_to_cpu(endianess,sb)\
	({\
	 	sb->magic = ##endianess_to_cpu(sb->magic,64);\
		sb->block_size = ##endianess_to_cpu0(sb->block_size,32);\
		sb->inodes_count = ##endianess_to_cpu(sb->inodes_count,64);\
		sb->free_blocks = ##endianess_to_cpu(sb->free_blocks,64);\
	 })

#define cpu_inode_to(endianess,inode)\
	({\
	 	inode->mode = cpu_to_##endianess(inode->mode,64);\
	 	inode->inode_no = cpu_to_##endianess(inode->inode_no,64);\
	 	inode->data_block_no = cpu_to_##endianess(inode->data_block_no,64);\
	 	inode->file_size = cpu_to_##endianess(inode->file_size,64);\
	 })
#define inode_to_cpu(endianess,inode)\
	({\
	 	inode->mode = ##endianess_to_cpu(inode->mode,64);\
	 	inode->inode_no = ##endianess_to_cpu(inode->inode_no,64);\
	 	inode->data_block_no = ##endianess_to_cpu(inode->data_block_no,64);\
	 	inode->file_size = ##endianess_to_cpuinode->file_size,64);\
	 })
