#include <simplefs-lib.h>

int32_t alloc_bmap(char *bitmap,int32_t bmap_len) {
	int32_t i = 0,j = 0;
	for(;i<bmap_len;i++) {
		if( (bitmap[i] & 0xff) !=0xff) {
			for(j=0;j<8;j++) {
				if( !(bitmap[i] & (1<<j))) {
					bitmap[i]|=(1<<j);
					break;
				}
			}
			return i*8 + j ; /*Bit numbers are starting from 0*/
		}
	}
return -1;
}

int free_bmap(char *bitmap,int32_t bmap_len, int loc) {
	int32_t i = 0,j = 0;
	char old_val;
	if (loc >= bmap_len * 8)
		return 0;
	i = loc / 8;
	j = loc % 8;
	old_val = bitmap[i] & (1<<j);
	bitmap[i] &= ~(1<<j);
	return old_val;
}
