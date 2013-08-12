#ifndef SIMPLEFS_LIB_H
#define SIMPLEFS_LIB_H
#ifndef __KERNEL__
#include <sys/types.h>
extern int32_t alloc_bmap(char *buffer,int32_t bmap_len);
#else
extern int32_t alloc_bmap(char *buffer,int32_t bmap_len);
#endif /*__KERNEL__*/
#endif /*SIMPLEFS_LIB_H*/
