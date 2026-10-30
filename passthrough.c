/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>
  Copyright (C) 2011       Sebastian Pipping <sebastian@pipping.org>

  This program can be distributed under the terms of the GNU GPLv2.
  See the file GPL2.txt.
*/

/** @file
 *
 * This file system mirrors the existing file system hierarchy of the
 * system, starting at the root file system. This is implemented by
 * just "passing through" all requests to the corresponding user-space
 * libc functions. Its performance is terrible.
 *
 * Compile with
 *
 *     gcc -Wall passthrough.c `pkg-config fuse3 --cflags --libs` -o passthrough
 *
 * ## Source code ##
 * \include passthrough.c
 */


#define FUSE_USE_VERSION FUSE_MAKE_VERSION(3, 18)

#define _GNU_SOURCE

#ifdef linux
/* For pread()/pwrite()/utimensat() */
#define _XOPEN_SOURCE 700
#endif

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <inttypes.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>
#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif

#include "passthrough_helpers.h"

static int fill_dir_plus = 0;
static int readdir_zero_ino;

/* Arquivo virtual: nao existe no disco e aparece apenas na raiz montada. */
#define STATUS_PATH "/status.txt"
#define STATUS_NAME "status.txt"

/* Atomicos permitem contar operacoes mesmo no modo multithread do FUSE. */
static atomic_uint_fast64_t directories_created;
static atomic_uint_fast64_t files_opened;
static atomic_uint_fast64_t read_requests;

struct status_snapshot {
	size_t length;
	char text[256];
};

static int is_status(const char *path)
{
	return path != NULL && strcmp(path, STATUS_PATH) == 0;
}

static void make_status(struct status_snapshot *snapshot)
{
	/* O texto e tres contadores de ate 20 digitos cabem em 256 bytes. */
	snapshot->length = (size_t)snprintf(snapshot->text, sizeof(snapshot->text),
		"Meu filesystem FUSE\n"
		"Diretorios criados com sucesso: %" PRIuFAST64 "\n"
		"Arquivos reais abertos com sucesso: %" PRIuFAST64 "\n"
		"Pedidos de leitura de arquivos reais: %" PRIuFAST64 "\n",
		atomic_load(&directories_created),
		atomic_load(&files_opened),
		atomic_load(&read_requests));
}

static const struct status_snapshot *get_status(struct fuse_file_info *fi,
					       struct status_snapshot *temporary)
{
	if (fi != NULL && fi->fh != 0)
		return (const struct status_snapshot *)(uintptr_t)fi->fh;
	make_status(temporary);
	return temporary;
}

static void status_getattr(struct stat *st, struct fuse_file_info *fi)
{
	struct status_snapshot temporary;
	const struct status_snapshot *snapshot = get_status(fi, &temporary);

	memset(st, 0, sizeof(*st));
	st->st_mode = S_IFREG | 0444;
	st->st_nlink = 1;
	st->st_uid = getuid();
	st->st_gid = getgid();
	st->st_size = (off_t)snapshot->length;
	st->st_atime = st->st_mtime = st->st_ctime = time(NULL);
}

static void *xmp_init(struct fuse_conn_info *conn,
		      struct fuse_config *cfg)
{
	(void) conn;
	/* Always replies inline on the io-uring worker thread */
	//fuse_set_conn_flag(conn, FUSE_CONN_FLAG_SINGLE_ISSUER);

	cfg->use_ino = !readdir_zero_ino;

	/* parallel_direct_writes feature depends on direct_io features.
	   To make parallel_direct_writes valid, need either set cfg->direct_io
	   in current function (recommended in high level API) or set fi->direct_io
	   in xmp_create() or xmp_open(). */
	// cfg->direct_io = 1;
	   cfg->parallel_direct_writes = 1;

	/* Pick up changes from lower filesystem right away. This is
	   also necessary for better hardlink support. When the kernel
	   calls the unlink() handler, it does not know the inode of
	   the to-be-removed entry and can therefore not invalidate
	   the cache of the associated inode - resulting in an
	   incorrect st_nlink value being reported for any remaining
	   hardlinks to this inode. */
	if (!cfg->auto_cache) {
		cfg->entry_timeout = 0;
		cfg->attr_timeout = 0;
		cfg->negative_timeout = 0;
	}

	return NULL;
}

static int xmp_getattr(const char *path, struct stat *stbuf,
		       struct fuse_file_info *fi)
{
	if (is_status(path)) {
		status_getattr(stbuf, fi);
		return 0;
	}

	(void) fi;
	int res;

	res = lstat(path, stbuf);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_access(const char *path, int mask)
{
	if (is_status(path))
		return (mask & (W_OK | X_OK)) ? -EACCES : 0;

	int res;

	res = access(path, mask);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_readlink(const char *path, char *buf, size_t size)
{
	if (is_status(path))
		return -EINVAL;

	int res;

	res = readlink(path, buf, size - 1);
	if (res == -1)
		return -errno;

	buf[res] = '\0';
	return 0;
}


static int xmp_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		       off_t offset, struct fuse_file_info *fi,
		       enum fuse_readdir_flags flags)
{
	DIR *dp;
	struct dirent *de;

	(void) offset;
	(void) fi;
	(void) flags;

	dp = opendir(path);
	if (dp == NULL)
		return -errno;

	if (strcmp(path, "/") == 0) {
		struct stat st;
		status_getattr(&st, NULL);
		if (filler(buf, STATUS_NAME, &st, 0, fill_dir_plus)) {
			closedir(dp);
			return 0;
		}
	}

	while ((de = readdir(dp)) != NULL) {
		if (strcmp(path, "/") == 0 && strcmp(de->d_name, STATUS_NAME) == 0)
			continue;
		struct stat st;
		if (fill_dir_plus) {
			fstatat(dirfd(dp), de->d_name, &st,
				AT_SYMLINK_NOFOLLOW);
		} else {
			memset(&st, 0, sizeof(st));
			st.st_ino = de->d_ino;
			st.st_mode = de->d_type << 12;
		}
		if (readdir_zero_ino)
			st.st_ino = 0;
		if (filler(buf, de->d_name, &st, 0, fill_dir_plus))
			break;
	}

	closedir(dp);
	return 0;
}

static int xmp_mknod(const char *path, mode_t mode, dev_t rdev)
{
	if (is_status(path))
		return -EEXIST;

	int res;

	res = mknod_wrapper(AT_FDCWD, path, NULL, mode, rdev);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_mkdir(const char *path, mode_t mode)
{
	if (is_status(path))
		return -EEXIST;

	int res;

	res = mkdir(path, mode);

	if (res == -1)
		return -errno;

	atomic_fetch_add(&directories_created, 1);
	return 0;
}

static int xmp_unlink(const char *path)
{
	if (is_status(path))
		return -EROFS;

	int res;

	res = unlink(path);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_rmdir(const char *path)
{
	if (is_status(path))
		return -ENOTDIR;

	int res;

	res = rmdir(path);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_symlink(const char *from, const char *to)
{
	if (is_status(to))
		return -EEXIST;

	int res;

	res = symlink(from, to);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_rename(const char *from, const char *to, unsigned int flags)
{
	if (is_status(from) || is_status(to))
		return -EROFS;

	int res;

	if (flags)
		return -EINVAL;

	res = rename(from, to);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_link(const char *from, const char *to)
{
	if (is_status(from) || is_status(to))
		return -EROFS;

	int res;

	res = link(from, to);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_chmod(const char *path, mode_t mode,
		     struct fuse_file_info *fi)
{
	if (is_status(path))
		return -EROFS;

	(void) fi;
	int res;

	res = chmod(path, mode);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_chown(const char *path, uid_t uid, gid_t gid,
		     struct fuse_file_info *fi)
{
	if (is_status(path))
		return -EROFS;

	(void) fi;
	int res;

	res = lchown(path, uid, gid);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_truncate(const char *path, off_t size,
			struct fuse_file_info *fi)
{
	if (is_status(path))
		return -EROFS;

	int res;

	if (fi != NULL)
		res = ftruncate(fi->fh, size);
	else
		res = truncate(path, size);
	if (res == -1)
		return -errno;

	return 0;
}

#ifdef HAVE_UTIMENSAT
static int xmp_utimens(const char *path, const struct timespec ts[2],
		       struct fuse_file_info *fi)
{
	if (is_status(path))
		return -EROFS;

	(void) fi;
	int res;

	/* don't use utime/utimes since they follow symlinks */
	res = utimensat(0, path, ts, AT_SYMLINK_NOFOLLOW);
	if (res == -1)
		return -errno;

	return 0;
}
#endif

static int xmp_create(const char *path, mode_t mode,
		      struct fuse_file_info *fi)
{
	if (is_status(path))
		return -EEXIST;

	int res;

	res = open(path, fi->flags, mode);
	if (res == -1)
		return -errno;

	fi->fh = res;
	atomic_fetch_add(&files_opened, 1);
	return 0;
}

static int xmp_open(const char *path, struct fuse_file_info *fi)
{
	if (is_status(path)) {
		struct status_snapshot *snapshot;
		if ((fi->flags & O_ACCMODE) != O_RDONLY || (fi->flags & O_TRUNC))
			return -EACCES;
		snapshot = malloc(sizeof(*snapshot));
		if (snapshot == NULL)
			return -ENOMEM;
		make_status(snapshot);
		/* Guardamos o texto por abertura, em vez de um descritor real. */
		fi->fh = (uint64_t)(uintptr_t)snapshot;
		/* Cada nova abertura consulta os contadores, sem cache de dados. */
		fi->direct_io = 1;
		fi->keep_cache = 0;
		return 0;
	}

	int res;

#ifdef __FreeBSD__
	/* During buffered write, the kernel may issue a READ request. */
	if (!(fi->flags & O_DIRECT) && (fi->flags & O_ACCMODE) == O_WRONLY) {
		fi->flags &= ~O_ACCMODE;
		fi->flags |= O_RDWR;
	}
#endif

	res = open(path, fi->flags);
	if (res == -1)
		return -errno;

        /* Enable direct_io when open has flags O_DIRECT to enjoy the feature
        parallel_direct_writes (i.e., to get a shared lock, not exclusive lock,
        for writes to the same file). */
	if (fi->flags & O_DIRECT) {
		fi->direct_io = 1;
		fi->parallel_direct_writes = 1;
	}

	fi->fh = res;
	atomic_fetch_add(&files_opened, 1);
	return 0;
}

static int xmp_read(const char *path, char *buf, size_t size, off_t offset,
		    struct fuse_file_info *fi)
{
	if (is_status(path)) {
		struct status_snapshot temporary;
		const struct status_snapshot *snapshot = get_status(fi, &temporary);
		if (offset < 0)
			return -EINVAL;
		if ((uintmax_t)offset >= snapshot->length)
			return 0; /* Fim do arquivo. */
		if (size > snapshot->length - (size_t)offset)
			size = snapshot->length - (size_t)offset;
		memcpy(buf, snapshot->text + (size_t)offset, size);
		return (int)size;
	}
	/* Conta pedidos ao callback, inclusive os que falham. */
	atomic_fetch_add(&read_requests, 1);

	int fd;
	int res;

	if(fi == NULL)
		fd = open(path, O_RDONLY);
	else
		fd = fi->fh;

	if (fd == -1)
		return -errno;

	res = pread(fd, buf, size, offset);
	if (res == -1)
		res = -errno;

	if(fi == NULL)
		close(fd);
	return res;
}

static int xmp_write(const char *path, const char *buf, size_t size,
		     off_t offset, struct fuse_file_info *fi)
{
	if (is_status(path))
		return -EROFS;

	int fd;
	int res;

	(void) fi;
	if(fi == NULL)
		fd = open(path, O_WRONLY);
	else
		fd = fi->fh;

	if (fd == -1)
		return -errno;

	res = pwrite(fd, buf, size, offset);
	if (res == -1)
		res = -errno;

	if(fi == NULL)
		close(fd);
	return res;
}

static int xmp_statfs(const char *path, struct statvfs *stbuf)
{
	if (is_status(path))
		path = "/";

	int res;

	res = statvfs(path, stbuf);
	if (res == -1)
		return -errno;

	return 0;
}

static int xmp_release(const char *path, struct fuse_file_info *fi)
{
	if (is_status(path)) {
		free((void *)(uintptr_t)fi->fh);
		fi->fh = 0;
		return 0;
	}

	(void) path;
	close(fi->fh);
	return 0;
}

static int xmp_fsync(const char *path, int isdatasync,
		     struct fuse_file_info *fi)
{
	/* Just a stub.	 This method is optional and can safely be left
	   unimplemented */

	(void) path;
	(void) isdatasync;
	(void) fi;
	return 0;
}

static int xmp_fallocate(const char *path, int mode,
			off_t offset, off_t length, struct fuse_file_info *fi)
{
	if (is_status(path))
		return -EROFS;

	int fd;
	int res;

	(void) fi;

	if(fi == NULL)
		fd = open(path, O_WRONLY);
	else
		fd = fi->fh;

	if (fd == -1)
		return -errno;

	res = do_fallocate(fd, mode, offset, length);

	if(fi == NULL)
		close(fd);
	return res;
}

#ifdef HAVE_SETXATTR
/* xattr operations are optional and can safely be left unimplemented */
static int xmp_setxattr(const char *path, const char *name, const char *value,
			size_t size, int flags)
{
	if (is_status(path))
		return -EROFS;

	int res = lsetxattr(path, name, value, size, flags);
	if (res == -1)
		return -errno;
	return 0;
}

static int xmp_getxattr(const char *path, const char *name, char *value,
			size_t size)
{
	if (is_status(path))
		return -ENODATA;

	int res = lgetxattr(path, name, value, size);
	if (res == -1)
		return -errno;
	return res;
}

static int xmp_listxattr(const char *path, char *list, size_t size)
{
	if (is_status(path))
		return 0;

	int res = llistxattr(path, list, size);
	if (res == -1)
		return -errno;
	return res;
}

static int xmp_removexattr(const char *path, const char *name)
{
	if (is_status(path))
		return -EROFS;

	int res = lremovexattr(path, name);
	if (res == -1)
		return -errno;
	return 0;
}
#endif /* HAVE_SETXATTR */

#ifdef HAVE_COPY_FILE_RANGE
static ssize_t xmp_copy_file_range(const char *path_in,
				   struct fuse_file_info *fi_in,
				   off_t offset_in, const char *path_out,
				   struct fuse_file_info *fi_out,
				   off_t offset_out, size_t len, int flags)
{
	if (is_status(path_out))
		return -EROFS;
	if (is_status(path_in))
		return -EOPNOTSUPP; /* Permite ao cliente copiar usando read/write. */

	int fd_in, fd_out;
	ssize_t res;

	if(fi_in == NULL)
		fd_in = open(path_in, O_RDONLY);
	else
		fd_in = fi_in->fh;

	if (fd_in == -1)
		return -errno;

	if(fi_out == NULL)
		fd_out = open(path_out, O_WRONLY);
	else
		fd_out = fi_out->fh;

	if (fd_out == -1) {
		close(fd_in);
		return -errno;
	}

	res = copy_file_range(fd_in, &offset_in, fd_out, &offset_out, len,
			      flags);
	if (res == -1)
		res = -errno;

	if (fi_out == NULL)
		close(fd_out);
	if (fi_in == NULL)
		close(fd_in);

	return res;
}
#endif

static off_t xmp_lseek(const char *path, off_t off, int whence, struct fuse_file_info *fi)
{
	if (is_status(path)) {
		struct status_snapshot temporary;
		const struct status_snapshot *snapshot = get_status(fi, &temporary);
		if (off < 0)
			return -EINVAL;
		if ((uintmax_t)off >= snapshot->length)
			return -ENXIO;
		if (whence == SEEK_DATA)
			return off;
		if (whence == SEEK_HOLE)
			return (off_t)snapshot->length;
		return -EINVAL;
	}

	int fd;
	off_t res;

	if (fi == NULL)
		fd = open(path, O_RDONLY);
	else
		fd = fi->fh;

	if (fd == -1)
		return -errno;

	res = lseek(fd, off, whence);
	if (res == -1)
		res = -errno;

	if (fi == NULL)
		close(fd);
	return res;
}

#ifdef HAVE_STATX
static int xmp_statx(const char *path, int flags, int mask, struct statx *stxbuf,
		     struct fuse_file_info *fi)
{
	if (is_status(path)) {
		struct stat st;
		status_getattr(&st, fi);
		memset(stxbuf, 0, sizeof(*stxbuf));
		stxbuf->stx_mask = STATX_TYPE | STATX_MODE | STATX_NLINK |
			STATX_UID | STATX_GID | STATX_SIZE | STATX_ATIME |
			STATX_MTIME | STATX_CTIME;
		stxbuf->stx_mode = st.st_mode;
		stxbuf->stx_nlink = st.st_nlink;
		stxbuf->stx_uid = st.st_uid;
		stxbuf->stx_gid = st.st_gid;
		stxbuf->stx_size = st.st_size;
		stxbuf->stx_atime.tv_sec = st.st_atime;
		stxbuf->stx_mtime.tv_sec = st.st_mtime;
		stxbuf->stx_ctime.tv_sec = st.st_ctime;
		return 0;
	}

	int fd = -1;
	int res;

	if (fi)
		fd = fi->fh;

	res = statx(fd, path, flags | AT_SYMLINK_NOFOLLOW, mask, stxbuf);
	if (res == -1)
		return -errno;

	return 0;
}
#endif

static const struct fuse_operations xmp_oper = {
	.init           = xmp_init,
	.getattr	= xmp_getattr,
	.access		= xmp_access,
	.readlink	= xmp_readlink,
	.readdir	= xmp_readdir,
	.mknod		= xmp_mknod,
	.mkdir		= xmp_mkdir,
	.symlink	= xmp_symlink,
	.unlink		= xmp_unlink,
	.rmdir		= xmp_rmdir,
	.rename		= xmp_rename,
	.link		= xmp_link,
	.chmod		= xmp_chmod,
	.chown		= xmp_chown,
	.truncate	= xmp_truncate,
#ifdef HAVE_UTIMENSAT
	.utimens	= xmp_utimens,
#endif
	.open		= xmp_open,
	.create 	= xmp_create,
	.read		= xmp_read,
	.write		= xmp_write,
	.statfs		= xmp_statfs,
	.release	= xmp_release,
	.fsync		= xmp_fsync,
	.fallocate	= xmp_fallocate,
#ifdef HAVE_SETXATTR
	.setxattr	= xmp_setxattr,
	.getxattr	= xmp_getxattr,
	.listxattr	= xmp_listxattr,
	.removexattr	= xmp_removexattr,
#endif
#ifdef HAVE_COPY_FILE_RANGE
	.copy_file_range = xmp_copy_file_range,
#endif
	.lseek		= xmp_lseek,
#ifdef HAVE_STATX
	.statx		= xmp_statx,
#endif
};

int main(int argc, char *argv[])
{
	enum { MAX_ARGS = 10 };
	int i,new_argc;
	char *new_argv[MAX_ARGS];

	umask(0);
			/* Process the "--plus" option apart */
	for (i=0, new_argc=0; (i<argc) && (new_argc<MAX_ARGS); i++) {
		if (!strcmp(argv[i], "--plus")) {
			fill_dir_plus = FUSE_FILL_DIR_PLUS;
		} else if (!strcmp(argv[i], "--readdir-zero-inodes")) {
			// Return zero inodes from readdir
			readdir_zero_ino = 1;
		} else {
			new_argv[new_argc++] = argv[i];
		}
	}
	return fuse_main(new_argc, new_argv, &xmp_oper, NULL);
}
