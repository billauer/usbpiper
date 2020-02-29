#ifndef _CUSE_H
#define _CUSE_H

#include <stdint.h>

/**
 * CUSE INIT request/reply flags
 *
 * CUSE_UNRESTRICTED_IOCTL:  use unrestricted ioctl
 */
#define CUSE_UNRESTRICTED_IOCTL	(1 << 0)

/**
 * Release flags
 */
#define FUSE_RELEASE_FLUSH	(1 << 0)
#define FUSE_RELEASE_FLOCK_UNLOCK	(1 << 1)

/**
 * Poll flags
 *
 * FUSE_POLL_SCHEDULE_NOTIFY: request poll notify
 */
#define FUSE_POLL_SCHEDULE_NOTIFY (1 << 0)

enum fuse_opcode {
	FUSE_LOOKUP		= 1,
	FUSE_FORGET		= 2,  /* no reply */
	FUSE_GETATTR		= 3,
	FUSE_SETATTR		= 4,
	FUSE_READLINK		= 5,
	FUSE_SYMLINK		= 6,
	FUSE_MKNOD		= 8,
	FUSE_MKDIR		= 9,
	FUSE_UNLINK		= 10,
	FUSE_RMDIR		= 11,
	FUSE_RENAME		= 12,
	FUSE_LINK		= 13,
	FUSE_OPEN		= 14,
	FUSE_READ		= 15,
	FUSE_WRITE		= 16,
	FUSE_STATFS		= 17,
	FUSE_RELEASE		= 18,
	FUSE_FSYNC		= 20,
	FUSE_SETXATTR		= 21,
	FUSE_GETXATTR		= 22,
	FUSE_LISTXATTR		= 23,
	FUSE_REMOVEXATTR	= 24,
	FUSE_FLUSH		= 25,
	FUSE_INIT		= 26,
	FUSE_OPENDIR		= 27,
	FUSE_READDIR		= 28,
	FUSE_RELEASEDIR		= 29,
	FUSE_FSYNCDIR		= 30,
	FUSE_GETLK		= 31,
	FUSE_SETLK		= 32,
	FUSE_SETLKW		= 33,
	FUSE_ACCESS		= 34,
	FUSE_CREATE		= 35,
	FUSE_INTERRUPT		= 36,
	FUSE_BMAP		= 37,
	FUSE_DESTROY		= 38,
	FUSE_IOCTL		= 39,
	FUSE_POLL		= 40,
	FUSE_NOTIFY_REPLY	= 41,
	FUSE_BATCH_FORGET	= 42,
	FUSE_FALLOCATE		= 43,
	FUSE_READDIRPLUS	= 44,
	FUSE_RENAME2		= 45,
	FUSE_LSEEK		= 46,
	FUSE_COPY_FILE_RANGE	= 47,

	/* CUSE specific operations */
	CUSE_INIT		= 4096,
};

/**
 * Flags returned by the OPEN request
 *
 * FOPEN_DIRECT_IO: bypass page cache for this open file
 * FOPEN_KEEP_CACHE: don't invalidate the data cache on open
 * FOPEN_NONSEEKABLE: the file is not seekable
 * FOPEN_CACHE_DIR: allow caching this directory
 * FOPEN_STREAM: the file is stream-like (no file position at all)
 */
#define FOPEN_DIRECT_IO		(1 << 0)
#define FOPEN_KEEP_CACHE	(1 << 1)
#define FOPEN_NONSEEKABLE	(1 << 2)
#define FOPEN_CACHE_DIR		(1 << 3)
#define FOPEN_STREAM		(1 << 4)

enum fuse_notify_code {
	FUSE_NOTIFY_POLL   = 1,
	FUSE_NOTIFY_INVAL_INODE = 2,
	FUSE_NOTIFY_INVAL_ENTRY = 3,
	FUSE_NOTIFY_STORE = 4,
	FUSE_NOTIFY_RETRIEVE = 5,
	FUSE_NOTIFY_DELETE = 6,
	FUSE_NOTIFY_CODE_MAX,
};

struct fuse_open_in {
	uint32_t	flags;
	uint32_t	unused;
};

struct fuse_open_out {
	uint64_t	fh;
	uint32_t	open_flags;
	uint32_t	padding;
};

struct fuse_release_in {
	uint64_t	fh;
	uint32_t	flags;
	uint32_t	release_flags;
	uint64_t	lock_owner;
};

struct fuse_read_in {
	uint64_t	fh;
	uint64_t	offset;
	uint32_t	size;
	uint32_t	read_flags;
	uint64_t	lock_owner;
	uint32_t	flags;
	uint32_t	padding;
};

struct fuse_write_in {
	uint64_t	fh;
	uint64_t	offset;
	uint32_t	size;
	uint32_t	write_flags;
	uint64_t	lock_owner;
	uint32_t	flags;
	uint32_t	padding;
};

struct fuse_write_out {
	uint32_t	size;
	uint32_t	padding;
};

struct cuse_init_in {
	uint32_t	major;
	uint32_t	minor;
	uint32_t	unused;
	uint32_t	flags;
};

struct cuse_init_out {
	uint32_t	major;
	uint32_t	minor;
	uint32_t	unused;
	uint32_t	flags;
	uint32_t	max_read;
	uint32_t	max_write;
	uint32_t	dev_major;		/* chardev major */
	uint32_t	dev_minor;		/* chardev minor */
	uint32_t	spare[10];
};

struct fuse_interrupt_in {
	uint64_t	unique;
};

struct fuse_poll_in {
	uint64_t	fh;
	uint64_t	kh;
	uint32_t	flags;
	uint32_t	events;
};

struct fuse_poll_out {
	uint32_t	revents;
	uint32_t	padding;
};

struct fuse_notify_poll_wakeup_out {
	uint64_t	kh;
};

struct fuse_in_header {
	uint32_t	len;
	uint32_t	opcode;
	uint64_t	unique;
	uint64_t	nodeid;
	uint32_t	uid;
	uint32_t	gid;
	uint32_t	pid;
	uint32_t	padding;
};

struct fuse_out_header {
	uint32_t	len;
	int32_t		error;
	uint64_t	unique;
};

#endif /* _CUSE_H */
