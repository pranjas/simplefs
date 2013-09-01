#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stropts.h>

#include <simple.h>
#include <simplefs-lib.h>
#include <inttypes.h>
#include <linux/fs.h>

#define VERSION			2
#define DEFAULT_PERC_INODES	10

#define SIMPLEFS_INODE_SIZE	(sizeof(struct simplefs_inode))


int main(int argc, char *argv[])
{
	int fd, nbytes,i;
	uint64_t nr_blocks;
	uint64_t nr_inodes;
	uint16_t nr_inodes_per_block;
	uint16_t nr_bits_per_block;
	uint16_t nr_blocks_written = 0;
	uint64_t block_dev_size = 0;

	ssize_t ret = 0;
	struct stat devinfo;
	struct simplefs_super_block sb;
	struct simplefs_inode root_inode;
	struct simplefs_inode welcomefile_inode;

	char welcomefile_name[] = "vanakkam";
	char welcomefile_body[] = "Love is God. God is Love. Anbe Murugan.\n";
	const uint64_t WELCOMEFILE_INODE_NUMBER = 2;
	char *buffer = NULL;

	struct simplefs_dir_record record;
	printf(" mkfs-simplefs\n Version %d\n Author: Pranay Kr. Srivastava\n",VERSION);
	printf(" ----------------------------------------------------------------------\n");
	printf(" Setting block size to %d\n",SIMPLEFS_DEFAULT_BLOCK_SIZE); 

	if (argc != 2) {
		printf("Usage: mkfs-simplefs <device>\n");
		return -1;
	}

	if(lstat(argv[1],&devinfo)) {
		perror("Error geting device information\n");
		return EXIT_FAILURE;
	}

	if( !(devinfo.st_mode & S_IFBLK) ) {
		printf("%s not a block device!. Exiting...\n",argv[1]);
		return EXIT_FAILURE;
	}

	fd = open(argv[1], O_RDWR);
	if (fd < 0) {
		perror("Error opening the device");
		return -1;
	}

	/* Begin writing of Block 0 - Super Block */
#ifdef __BIG_ENDIAN_
	sb.char_version[0] = SIMPLEFS_ENDIANESS_BIG;
#else
	sb.char_version[0] = SIMPLEFS_ENDIANESS_LITTLE;
#endif
	sb.magic = SIMPLEFS_MAGIC;
	sb.block_size = SIMPLEFS_DEFAULT_BLOCK_SIZE;

	/* One inode for rootdirectory and another for a welcome file that we are going to create */
	sb.inodes_count = 2;

	/* FIXME: Free blocks management is not implemented yet */
	/*
	 * 	sb.free_blocks = ~0;
		sb.free_blocks &= ~(1 << WELCOMEFILE_DATABLOCK_NUMBER);
	*/

	if ( (ret = ioctl(fd,BLKGETSIZE64,&block_dev_size)) ) {
		perror("Error getting block device size:");
		goto exit;
	}
	nr_blocks = block_dev_size / sb.block_size;
	nr_inodes = nr_blocks/DEFAULT_PERC_INODES;
	nr_bits_per_block = sb.block_size * 8;
	nr_inodes_per_block = sb.block_size / SIMPLEFS_INODE_SIZE;
	buffer = calloc(1,sb.block_size);
	if(!buffer) {
		printf("Couldn't allocate enough memory. Exiting...\n");
		return EXIT_FAILURE;
	}

	if( (ret = lseek (fd,sb.block_size,SEEK_SET)) <0 ) {
		perror("Couldn't seek into device [SB]\n");
		goto exit;
	}
	sb.inode_block_start = ++nr_blocks_written;
	/* End of writing of Block 0 - Super block */

	/* Skip the inode table and move on to write the
	 * inode bitmap.
	 * */
	if (	(ret =lseek(fd,
		(nr_inodes/nr_inodes_per_block+
		 (nr_inodes % (nr_inodes_per_block )?1:0))*sb.block_size,SEEK_CUR)) <0 ) {
		perror("Error seeking to device \n");
		goto exit;
	}
	nr_blocks_written += (nr_inodes/nr_inodes_per_block +
		 		(nr_inodes % (nr_inodes_per_block )?1:0));
	sb.inode_bitmap_start = nr_blocks_written;

	/*Begin Writing inode bitmap table*/
	i = nr_inodes/nr_bits_per_block;
	do {
		if( (ret = write(fd,buffer,sb.block_size))!= sb.block_size) {
			perror("Error writing inode bitmap to device.\n");
			goto exit;
		}
		i -=nr_bits_per_block;
		nr_blocks_written++;
	}while(i>0);

	sb.block_bitmap_start = nr_blocks_written;

	/*Begin Writing the block bitmap table*/
	i = nr_blocks/nr_bits_per_block;
	do {
		if ( (ret = write(fd,buffer,sb.block_size)) != sb.block_size) {
			perror("Error writing block bitmap table");
			goto exit;
		}
		i -= nr_bits_per_block;
		nr_blocks_written++;
	}while(i>0);

	sb.data_block_start = nr_blocks_written;

	/* 
	 * Reusing buffer for writing inodes first.
	 * */
	memset(buffer,0,sb.block_size);

	if(  (ret = lseek(fd,sb.block_size * sb.inode_block_start,SEEK_SET)) < 0 ) {
		perror("Unable to seek into device [INODE]. Exiting..\n");
		goto exit;
	}

	
	/* Begin writing of Block 1 - Inode Store */
	root_inode.mode = S_IFDIR;
	root_inode.inode_no = SIMPLEFS_ROOTDIR_INODE_NUMBER;
	root_inode.data_block_number = /*SIMPLEFS_ROOTDIR_DATABLOCK_NUMBER*/nr_blocks_written++;
	root_inode.dir_children_count = 1;
	if(! (sb.char_version[0] & SIMPLEFS_ENDIANESS_LITTLE))
		cpu_inode_to(le,&root_inode);
	memcpy(buffer,&root_inode,SIMPLEFS_INODE_SIZE);

	welcomefile_inode.mode = S_IFREG;
	welcomefile_inode.inode_no = WELCOMEFILE_INODE_NUMBER;
	welcomefile_inode.data_block_number = nr_blocks_written++;
	welcomefile_inode.file_size = sizeof(welcomefile_body);
	if(! (sb.char_version[0] & SIMPLEFS_ENDIANESS_LITTLE))
		cpu_inode_to(le,&welcomefile_inode);
	memcpy(buffer+SIMPLEFS_INODE_SIZE,&welcomefile_inode,SIMPLEFS_INODE_SIZE);

	ret = write(fd,buffer,sb.block_size);

	if (ret != sb.block_size) {
		printf
		    ("The root/welcomefile inode was not written properly. Retry your mkfs\n");
		ret = -1;
		goto exit;
	}
	printf("root directory inode written succesfully\n");
	printf("welcomefile inode written succesfully\n");

	/* End of writing of Inodes in Inode Block  - inode Store */

	/*Set the inode bitmap to allocate two inodes*/
	if ( (ret = lseek(fd,sb.inode_bitmap_start * sb.block_size,SEEK_SET)) < 0 ) {
		printf("Couldn't seek into device [Inode Bitmap]...Exiting\n");
		goto exit;
	}
	memset(buffer,0,sb.block_size);
	if ( (ret=alloc_bmap(buffer,sb.block_size) + 1) != SIMPLEFS_ROOTDIR_INODE_NUMBER) {
		printf("Bug in bitmap allocator. Should be %d "
			"but was %zd .....Exiting\n",
			SIMPLEFS_ROOTDIR_INODE_NUMBER,ret);
		goto exit;
	}
	if ( (ret=alloc_bmap(buffer,sb.block_size) + 1) != WELCOMEFILE_INODE_NUMBER) {
		printf(" Bug in bitmap allocator. Should be %zd"
			" but was %zd ....Exiting\n",WELCOMEFILE_INODE_NUMBER,ret);
		goto exit;
	}

	if ( (ret = write(fd,buffer,sb.block_size)) != sb.block_size) {
		printf("Couldn't complete write for inode bitmap"
			" written %zd of %d bytes\n",ret,sb.block_size);
		ret = -1;
		goto exit;
	}

	/* 
	 * Set the number of blocks we have taken in block bitmap.
	 **/
	i = nr_blocks_written;
	if( (ret=lseek(fd,sb.block_bitmap_start * sb.block_size,SEEK_SET)) < 0) {
		perror("Unable to seek into device [Block Bitmap] \n");
		goto exit;
	}
	while (i) {
		memset(buffer,0,sb.block_size);
		while (i && alloc_bmap(buffer,sb.block_size)>=0)
			i--;
		if( (ret=write(fd,buffer,sb.block_size) !=sb.block_size)) {
			printf("Couldn't complete write for block bitmap"
				" written %zd of %d bytes\n",ret,sb.block_size);
			ret = -1;
			goto exit;
		}
	}

	/* Begin writing of Data Block  - Root Directory datablocks */
	if( (ret=lseek(fd, root_inode.data_block_number * sb.block_size, SEEK_SET)) < 0 ) {
		perror("Unable to seek into device [Data Block root inode]\n");
		goto exit;
	}
	memset(buffer,0,sb.block_size);	
	strcpy(record.filename, welcomefile_name);
	record.inode_no = WELCOMEFILE_INODE_NUMBER;
	record.name_len = strlen(record.filename);
	if(! (sb.char_version[0] & SIMPLEFS_ENDIANESS_LITTLE)) {
		record.inode_no = cpu_to_le(record.inode_no,64);
	}
	nbytes = sizeof(record);
	memcpy(buffer,&record,dir_record_len(&record));

	if ( (ret = write(fd, buffer,dir_record_len(&record))) != dir_record_len(&record) ) {
		printf
		    ("Writing the rootdirectory datablock (name+inode_no pair for welcomefile) has failed\n");
		ret = -1;
		goto exit;
	}
	printf
	    ("root directory datablocks (name+inode_no pair for welcomefile) written succesfully\n");
	/* End of writing of Block 2 - Root directory contents */

	/* Begin writing of Block 3 - Welcome file contents */
	nbytes = sizeof(welcomefile_body);
	memcpy(buffer,welcomefile_body,nbytes);

	if( (ret = lseek(fd,welcomefile_inode.data_block_number * sb.block_size,SEEK_SET)) < 0){
		perror("Unable to seek into device [Data block welcome file]\n");
		goto exit;
	}

	ret = write(fd, buffer, nbytes);
	if (ret != nbytes) {
		printf("Writing welcomefile body has failed\n");
		ret = -1;
		goto exit;
	}
	printf("welcomefilebody has been written succesfully\n");

	/*Finally write the super block*/
	sb.free_blocks = nr_blocks - nr_blocks_written;

	if ( (ret = lseek(fd , 0 , SEEK_SET)) < 0){
		perror("Couldn't seek into device [SB Finalize]\n");
		goto exit;
	}
	memset(buffer,0,sb.block_size);
	if(! (sb.char_version[0] & SIMPLEFS_ENDIANESS_LITTLE)) {
		cpu_super_to(le,&sb);
	}
	memcpy(buffer,&sb,sizeof(sb));
	if ( (ret = write(fd,buffer,sb.block_size)) != sb.block_size) {
		printf ("Couldn't complete write of super block"
			" written %zd of %d bytes\n",ret,sb.block_size);
		goto exit;
	}
	/* End of writing of Block 3 - Welcome file contents */
	ret = 0;
	if(! (sb.char_version[0] & SIMPLEFS_ENDIANESS_LITTLE)) {
		super_to_cpu(le,&sb);
	}
	printf ("Total blocks on device %s = %zd\n",argv[1],nr_blocks);
	printf ("Total inodes on device %s = %zd\n",argv[1],nr_inodes);
	printf ("Free blocks available on device %s = %zd\n",argv[1],sb.free_blocks);
	printf ("Inode block on device %s starts from block number %zd\n",argv[1],sb.inode_block_start);
	printf ("Inode bitmap on device %s start from block number %zd\n",argv[1],sb.inode_bitmap_start);
	printf ("Block bitmap on device %s start from block number %zd\n",argv[1],sb.block_bitmap_start);
	printf ("Data Blocks on device %s start from block number %zd\n",argv[1],sb.data_block_start);
exit:
	close(fd);
	free(buffer);
	return ret;
}
