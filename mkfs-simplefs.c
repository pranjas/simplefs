#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "simple.h"

#define VERSION			1
#define DEFAULT_PERC_INODES	10

int main(int argc, char *argv[])
{
	int fd, nbytes,i;
	uint64_t nr_blocks;
	uint64_t nr_inodes;
	uint16_t nr_inodes_per_block;
	uint16_t nr_bits_per_block;

	ssize_t ret;
	struct stat devinfo;
	struct simplefs_super_block sb;
	struct simplefs_inode root_inode;
	struct simplefs_inode welcomefile_inode;

	char welcomefile_name[] = "vanakkam";
	char welcomefile_body[] = "Love is God. God is Love. Anbe Murugan.\n";
	const uint64_t WELCOMEFILE_INODE_NUMBER = 2;
	const uint64_t WELCOMEFILE_DATABLOCK_NUMBER = 3;
	char *buffer = NULL;

	char *block_padding;

	struct simplefs_dir_record record;

	if (argc != 2) {
		printf("Usage: mkfs-simplefs <device>\n");
		return -1;
	}

	if(lstat(argv[1],&devinfo)) {
		perror("Error geting device information\n");
		return EXIT_FAILURE;
	}

	if( !(devinfo->st_mode & S_IFBLK) ) {
		printf("%s not a block device!. Exiting...\n",argv[1]);
		return EXIT_FAILURE;
	}

	fd = open(argv[1], O_RDWR);
	if (fd < 0) {
		perror("Error opening the device");
		return -1;
	}

	/* Begin writing of Block 0 - Super Block */
#ifdef __BIG_ENDIAN__
	sb.version = cpu_to_le((VERSION<<1)|SIMPLEFS_ENDIANESS_BIG,32);
#else
	sb.version = (VERSION<<1)|SIMPLEFS_ENDIANESS_LITTLE;
#endif
	sb.magic = SIMPLEFS_MAGIC;
	sb.block_size = SIMPLEFS_DEFAULT_BLOCK_SIZE;

	/* One inode for rootdirectory and another for a welcome file that we are going to create */
	sb.inodes_count = 2;

	/* FIXME: Free blocks management is not implemented yet */
	sb.free_blocks = ~0;
	sb.free_blocks &= ~(1 << WELCOMEFILE_DATABLOCK_NUMBER);

	nr_blocks = devinfo.st_size / sb.block_size;
	nr_inodes = nr_blocks/DEFAULT_PREC_INODES;
	nr_bits_per_block = sb.block_size * 8;
	nr_inodes_per_block = sb.block_size / sizeof(struct simplefs_inode);
	buffer = calloc(1,sb.block_size);
	if(!buffer) {
		printf("Couldn't allocate enough memory. Exiting...\n");
		return EXIT_FAILURE;
	}

	ret = write(fd, (char *)&sb, sizeof(sb));

	if (ret != SIMPLEFS_DEFAULT_BLOCK_SIZE) {
		printf
		    ("bytes written [%d] are not equal to the default block size\n",
		     (int)ret);
		ret = -1;
		goto exit;
	}

	printf("Super block written succesfully\n");
	/* End of writing of Block 0 - Super block */

	/* Skip the inode table and move on to write the
	 * inode bitmap.
	 * */
	if (lseek(fd,
		(nr_inodes/nr_inodes_per_block+
		 (nr_inodes % (nr_inodes_per_blocks )?1:0))*sb.block_size,SEEK_CUR) ) {

		perror("Error seeking to device \n");
		goto exit;
	}
	/*Begin Writing inode bitmap table*/
	i = nr_inodes/nr_bits_per_block;
	do {
		if( (ret = write(fd,buffer,sb.block_size))!= sb.block_size) {
			perror("Error writing inode bitmap to device.\n");
			goto exit;
		}
		
		i -=nr_bits_per_block;
	}while(i>0);

	/*Begin Writing the block bitmap table*/
	i = nr_blocks/nr_bits_per_block;
	do {
		if ( (ret = write(fd,buffer,sb.block_size)) != sb.block_size) {
			perror("Error writing block bitmap table");
			goto exit;
		}
		i -= nr_bits_per_block;
	}while(i>0);

	/* Begin writing of Block 1 - Inode Store */
	root_inode.mode = S_IFDIR;
	root_inode.inode_no = SIMPLEFS_ROOTDIR_INODE_NUMBER;
	root_inode.data_block_number = SIMPLEFS_ROOTDIR_DATABLOCK_NUMBER;
	root_inode.dir_children_count = 1;

	ret = write(fd, (char *)&root_inode, sizeof(root_inode));

	if (ret != sizeof(root_inode)) {
		printf
		    ("The inode store was not written properly. Retry your mkfs\n");
		ret = -1;
		goto exit;
	}
	printf("root directory inode written succesfully\n");

	welcomefile_inode.mode = S_IFREG;
	welcomefile_inode.inode_no = WELCOMEFILE_INODE_NUMBER;
	welcomefile_inode.data_block_number = WELCOMEFILE_DATABLOCK_NUMBER;
	welcomefile_inode.file_size = sizeof(welcomefile_body);
	ret = write(fd, (char *)&welcomefile_inode, sizeof(root_inode));

	if (ret != sizeof(root_inode)) {
		printf
		    ("The welcomefile inode was not written properly. Retry your mkfs\n");
		ret = -1;
		goto exit;
	}
	printf("welcomefile inode written succesfully\n");

	nbytes =
	    SIMPLEFS_DEFAULT_BLOCK_SIZE - sizeof(root_inode) -
	    sizeof(welcomefile_inode);
	block_padding = malloc(nbytes);

	ret = write(fd, block_padding, nbytes);

	if (ret != nbytes) {
		printf
		    ("The padding bytes are not written properly. Retry your mkfs\n");
		ret = -1;
		goto exit;
	}
	printf
	    ("inode store padding bytes (after the two inodes) written sucessfully\n");

	/* End of writing of Block 1 - inode Store */

	/* Begin writing of Block 2 - Root Directory datablocks */
	strcpy(record.filename, welcomefile_name);
	record.inode_no = WELCOMEFILE_INODE_NUMBER;
	nbytes = sizeof(record);

	ret = write(fd, (char *)&record, nbytes);
	if (ret != nbytes) {
		printf
		    ("Writing the rootdirectory datablock (name+inode_no pair for welcomefile) has failed\n");
		ret = -1;
		goto exit;
	}
	printf
	    ("root directory datablocks (name+inode_no pair for welcomefile) written succesfully\n");

	nbytes = SIMPLEFS_DEFAULT_BLOCK_SIZE - sizeof(record);
	block_padding = realloc(block_padding, nbytes);

	ret = write(fd, block_padding, nbytes);
	if (ret != nbytes) {
		printf
		    ("Writing the padding for rootdirectory children datablock has failed\n");
		ret = -1;
		goto exit;
	}
	printf
	    ("padding after the rootdirectory children written succesfully\n");
	/* End of writing of Block 2 - Root directory contents */

	/* Begin writing of Block 3 - Welcome file contents */
	nbytes = sizeof(welcomefile_body);
	ret = write(fd, welcomefile_body, nbytes);
	if (ret != nbytes) {
		printf("Writing welcomefile body has failed\n");
		ret = -1;
		goto exit;
	}
	printf("welcomefilebody has been written succesfully\n");
	/* End of writing of Block 3 - Welcome file contents */

	ret = 0;

exit:
	close(fd);
	free(buffer);
	return ret;
}
