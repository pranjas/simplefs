INCLUDE_DIRS= -I ../
EXTRA_CFLAGS= -O2 -Wall
CC=gcc
MKFS_SIMPLEFS_OBJS=mkfs-simplefs.o simplefs-lib.o
TARGETS=mkfs-simplefs
all: $(TARGETS)
	
mkfs-simplefs: mkfs-simplefs.o simplefs-lib.o
	$(CC)  $(MKFS_SIMPLEFS_OBJS) -o $@

clean:
	rm -f $(MKFS_SIMPLEFS_OBJS) $(TARGETS)
.c.o:
	$(CC) -c $(INCLUDE_DIRS) $(EXTRA_CFLAGS) $< -o $@

