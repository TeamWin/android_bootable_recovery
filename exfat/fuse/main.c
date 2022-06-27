/*
	main.c (01.09.09)
	FUSE-based exFAT implementation. Requires FUSE 2.6 or later.

	Free exFAT implementation.
	Copyright (C) 2010-2018  Andrew Nayenko

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License along
	with this program; if not, write to the Free Software Foundation, Inc.,
	51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include <exfat.h>
#define FUSE_USE_VERSION 26
#include <fuse.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <limits.h>
#include <sys/types.h>
#include <pwd.h>
#include <unistd.h>

#ifndef DEBUG
	#define exfat_debug(format, ...) do {} while (0)
#endif

#if !defined(FUSE_VERSION) || (FUSE_VERSION < 26)
	#error FUSE 2.6 or later is required
#endif

struct exfat ef;

static struct exfat_node* get_node(const struct fuse_file_info* fi)
{
	return (struct exfat_node*) (size_t) fi->fh;
}

static void set_node(struct fuse_file_info* fi, struct exfat_node* node)
{
	fi->fh = (uint64_t) (size_t) node;
	fi->keep_cache = 1;
}

static int fuse_exfat_getattr(const char* path, struct stat* stbuf)
{
	struct exfat_node* node;
	int rc;

	exfat_debug("[%s] %s", __func__, path);

	rc = exfat_lookup(&ef, &node, path);
	if (rc != 0)
		return rc;

	exfat_stat(&ef, node, stbuf);
	exfat_put_node(&ef, node);
	return 0;
}

static int fuse_exfat_truncate(const char* path, off_t size)
{
	struct exfat_node* node;
	int rc;

	exfat_debug("[%s] %s, %"PRId64, __func__, path, size);

	rc = exfat_lookup(&ef, &node, path);
	if (rc != 0)
		return rc;

	rc = exfat_truncate(&ef, node, size, true);
	if (rc != 0)
	{
		exfat_flush_node(&ef, node);	/* ignore return code */
		exfat_put_node(&ef, node);
		return rc;
	}
	rc = exfat_flush_node(&ef, node);
	exfat_put_node(&ef, node);
	return rc;
}

static int fuse_exfat_readdir(const char* path, void* buffer,
		fuse_fill_dir_t filler, UNUSED off_t offset,
		UNUSED struct fuse_file_info* fi)
{
	struct exfat_node* parent;
	struct exfat_node* node;
	struct exfat_iterator it;
	int rc;
	char name[EXFAT_UTF8_NAME_BUFFER_MAX];
	struct stat stbuf;

	exfat_debug("[%s] %s", __func__, path);

	rc = exfat_lookup(&ef, &parent, path);
	if (rc != 0)
		return rc;
	if (!(parent->attrib & EXFAT_ATTRIB_DIR))
	{
		exfat_put_node(&ef, parent);
		exfat_error("'%s' is not a directory (%#hx)", path, parent->attrib);
		return -ENOTDIR;
	}

	filler(buffer, ".", NULL, 0);
	filler(buffer, "..", NULL, 0);

	rc = exfat_opendir(&ef, parent, &it);
	if (rc != 0)
	{
		exfat_put_node(&ef, parent);
		exfat_error("failed to open directory '%s'", path);
		return rc;
	}
	while ((node = exfat_readdir(&it)))
	{
		exfat_get_name(node, name);
		exfat_debug("[%s] %s: %s, %"PRId64" bytes, cluster 0x%x", __func__,
				name, node->is_contiguous ? "contiguous" : "fragmented",
				node->size, node->start_cluster);
		exfat_stat(&ef, node, &stbuf);
		filler(buffer, name, &stbuf, 0);
		exfat_put_node(&ef, node);
	}
	exfat_closedir(&ef, &it);
	exfat_put_node(&ef, parent);
	return 0;
}

static int fuse_exfat_open(const char* path, struct fuse_file_info* fi)
{
	struct exfat_node* node;
	int rc;

	exfat_debug("[%s] %s", __func__, path);

	rc = exfat_lookup(&ef, &node, path);
	if (rc != 0)
		return rc;
	set_node(fi, node);
	return 0;
}

static int fuse_exfat_create(const char* path, UNUSED mode_t mode,
		struct fuse_file_info* fi)
{
	struct exfat_node* node;
	int rc;

	exfat_debug("[%s] %s 0%ho", __func__, path, mode);

	rc = exfat_mknod(&ef, path);
	if (rc != 0)
		return rc;
	rc = exfat_lookup(&ef, &node, path);
	if (rc != 0)
		return rc;
	set_node(fi, node);
	return 0;
}

static int fuse_exfat_release(UNUSED const char* path,
		struct fuse_file_info* fi)
{
	/*
	   This handler is called by FUSE on close() syscall. If the FUSE
	   implementation does not call flush handler, we will flush node here.
	   But in this case we will not be able to return an error to the caller.
	   See fuse_exfat_flush() below.
	*/
	exfat_debug("[%s] %s", __func__, path);
 	exfat_flush_node(&ef, get_node(fi));
	exfat_put_node(&ef, get_node(fi));
	return 0; /* FUSE ignores this return value */
}

static int fuse_exfat_flush(UNUSED const char* path, struct fuse_file_info* fi)
{
	/*
	   This handler may be called by FUSE on close() syscall. FUSE also deals
	   with removals of open files, so we don't free clusters on close but
	   only on rmdir and unlink. If the FUSE implementation does not call this
	   handler we will flush node on release. See fuse_exfat_relase() above.
	*/
	exfat_debug("[%s] %s", __func__, path);
	return exfat_flush_node(&ef, get_node(fi));
}

static int fuse_exfat_fsync(UNUSED const char* path, UNUSED int datasync,
		UNUSED struct fuse_file_info *fi)
{
	int rc;

	exfat_debug("[%s] %s", __func__, path);
	rc = exfat_flush_nodes(&ef);
	if (rc != 0)
		return rc;
	rc = exfat_flush(&ef);
	if (rc != 0)
		return rc;
	return exfat_fsync(ef.dev);
}

static int fuse_exfat_read(UNUSED const char* path, char* buffer,
		size_t size, off_t offset, struct fuse_file_info* fi)
{
	exfat_debug("[%s] %s (%zu bytes)", __func__, path, size);
	return exfat_generic_pread(&ef, get_node(fi), buffer, size, offset);
}

static int fuse_exfat_write(UNUSED const char* path, const char* buffer,
		size_t size, off_t offset, struct fuse_file_info* fi)
{
	exfat_debug("[%s] %s (%zu bytes)", __func__, path, size);
	return exfat_generic_pwrite(&ef, get_node(fi), buffer, size, offset);
}

static int fuse_exfat_unlink(const char* path)
{
	struct exfat_node* node;
	int rc;

	exfat_debug("[%s] %s", __func__, path);

	rc = exfat_lookup(&ef, &node, path);
	if (rc != 0)
		return rc;

	rc = exfat_unlink(&ef, node);
	exfat_put_node(&ef, node);
	if (rc != 0)
		return rc;
	return exfat_cleanup_node(&ef, node);
}

static int fuse_exfat_rmdir(const char* path)
{
	struct exfat_node* node;
	int rc;

	exfat_debug("[%s] %s", __func__, path);

	rc = exfat_lookup(&ef, &node, path);
	if (rc != 0)
		return rc;

	rc = exfat_rmdir(&ef, node);
	exfat_put_node(&ef, node);
	if (rc != 0)
		return rc;
	return exfat_cleanup_node(&ef, node);
}

static int fuse_exfat_mknod(const char* path, UNUSED mode_t mode,
		UNUSED dev_t dev)
{
	exfat_debug("[%s] %s 0%ho", __func__, path, mode);
	return exfat_mknod(&ef, path);
}

static int fuse_exfat_mkdir(const char* path, UNUSED mode_t mode)
{
	exfat_debug("[%s] %s 0%ho", __func__, path, mode);
	return exfat_mkdir(&ef, path);
}

static int fuse_exfat_rename(const char* old_path, const char* new_path)
{
	exfat_debug("[%s] %s => %s", __func__, old_path, new_path);
	return exfat_rename(&ef, old_path, new_path);
}

static int fuse_exfat_utimens(const char* path, const struct timespec tv[2])
{
	struct exfat_node* node;
	int rc;

	exfat_debug("[%s] %s", __func__, path);

	rc = exfat_lookup(&ef, &node, path);
	if (rc != 0)
		return rc;

	exfat_utimes(node, tv);
	rc = exfat_flush_node(&ef, node);
	exfat_put_node(&ef, node);
	return rc;
}

static int fuse_exfat_chmod(UNUSED const char* path, mode_t mode)
{
	const mode_t VALID_MODE_MASK = S_IFREG | S_IFDIR |
			S_IRWXU | S_IRWXG | S_IRWXO;

	exfat_debug("[%s] %s 0%ho", __func__, path, mode);
	if (mode & ~VALID_MODE_MASK)
		return -EPERM;
	return 0;
}

static int fuse_exfat_chown(UNUSED const char* path, uid_t uid, gid_t gid)
{
	exfat_debug("[%s] %s %u:%u", __func__, path, uid, gid);
	if (uid != ef.uid || gid != ef.gid)
		return -EPERM;
	return 0;
}

static int fuse_exfat_statfs(UNUSED const char* path, struct statvfs* sfs)
{
	exfat_debug("[%s]", __func__);

	sfs->f_bsize = CLUSTER_SIZE(*ef.sb);
	sfs->f_frsize = CLUSTER_SIZE(*ef.sb);
	sfs->f_blocks = le64_to_cpu(ef.sb->sector_count) >> ef.sb->spc_bits;
	sfs->f_bavail = exfat_count_free_clusters(&ef);
	sfs->f_bfree = sfs->f_bavail;
	sfs->f_namemax = EXFAT_NAME_MAX;

	/*
	   Below are fake values because in exFAT there is
	   a) no simple way to count files;
	   b) no such thing as inode;
	   So here we assume that inode = cluster.
	*/
	sfs->f_files = le32_to_cpu(ef.sb->cluster_count);
	sfs->f_favail = sfs->f_bfree >> ef.sb->spc_bits;
	sfs->f_ffree = sfs->f_bavail;

	return 0;
}

static void* fuse_exfat_init(struct fuse_conn_info* fci)
{
	exfat_debug("[%s]", __func__);
#ifdef FUSE_CAP_BIG_WRITES
	fci->want |= FUSE_CAP_BIG_WRITES;
#endif

	/* mark super block as dirty; failure isn't a big deal */
	exfat_soil_super_block(&ef);

	return NULL;
}

static void fuse_exfat_destroy(UNUSED void* unused)
{
	exfat_debug("[%s]", __func__);
	exfat_unmount(&ef);
}

static void usage(const char* prog)
{
	fprintf(stderr, "Usage: %s [-d] [-o options] [-V] <device> <dir>\n", prog);
	exit(1);
}

static struct fuse_operations fuse_exfat_ops =
{
	.getattr	= fuse_exfat_getattr,
	.truncate	= fuse_exfat_truncate,
	.readdir	= fuse_exfat_readdir,
	.open		= fuse_exfat_open,
	.create		= fuse_exfat_create,
	.release	= fuse_exfat_release,
	.flush		= fuse_exfat_flush,
	.fsync		= fuse_exfat_fsync,
	.fsyncdir	= fuse_exfat_fsync,
	.read		= fuse_exfat_read,
	.write		= fuse_exfat_write,
	.unlink		= fuse_exfat_unlink,
	.rmdir		= fuse_exfat_rmdir,
	.mknod		= fuse_exfat_mknod,
	.mkdir		= fuse_exfat_mkdir,
	.rename		= fuse_exfat_rename,
	.utimens	= fuse_exfat_utimens,
	.chmod		= fuse_exfat_chmod,
	.chown		= fuse_exfat_chown,
	.statfs		= fuse_exfat_statfs,
	.init		= fuse_exfat_init,
	.destroy	= fuse_exfat_destroy,
};

static char* add_option(char* options, const char* name, const char* value)
{
	size_t size;
	char* optionsf = options;

	if (value)
		size = strlen(options) + strlen(name) + strlen(value) + 3;
	else
		size = strlen(options) + strlen(name) + 2;

	options = realloc(options, size);
	if (options == NULL)
	{
		free(optionsf);
		exfat_error("failed to reallocate options string");
		return NULL;
	}
	strcat(options, ",");
	strcat(options, name);
	if (value)
	{
		strcat(options, "=");
		strcat(options, value);
	}
	return options;
}

static void escape(char* escaped, const char* orig)
{
	do
	{
		if (*orig == ',' || *orig == '\\')
			*escaped++ = '\\';
	}
	while ((*escaped++ = *orig++));
}

static char* add_fsname_option(char* options, const char* spec)
{
	/* escaped string cannot be more than twice as big as the original one */
	char* escaped = malloc(strlen(spec) * 2 + 1);

	if (escaped == NULL)
	{
		free(options);
		exfat_error("failed to allocate escaped string for %s", spec);
		return NULL;
	}

	/* on some platforms (e.g. Android, Solaris) device names can contain
	   commas */
	escape(escaped, spec);
	options = add_option(options, "fsname", escaped);
	free(escaped);
	return options;
}

static char* add_ro_option(char* options, bool ro)
{
	return ro ? add_option(options, "ro", NULL) : options;
}

#if defined(__linux__)
static char* add_user_option(char* options)
{
	struct passwd* pw;

	if (getuid() == 0)
		return options;

	pw = getpwuid(getuid());
	if (pw == NULL || pw->pw_name == NULL)
	{
		free(options);
		exfat_error("failed to determine username");
		return NULL;
	}
	return add_option(options, "user", pw->pw_name);
}
#endif

#if defined(__linux__)
static char* add_blksize_option(char* options, long cluster_size)
{
	long page_size = sysconf(_SC_PAGESIZE);
	char blksize[20];

	if (page_size < 1)
		page_size = 0x1000;

	snprintf(blksize, sizeof(blksize), "%ld", MIN(page_size, cluster_size));
	return add_option(options, "blksize", blksize);
}
#endif

static char* add_fuse_options(char* options, const char* spec, bool ro)
{
	options = add_fsname_option(options, spec);
	if (options == NULL)
		return NULL;
	options = add_ro_option(options, ro);
	if (options == NULL)
		return NULL;
#if defined(__linux__)
	options = add_user_option(options);
	if (options == NULL)
		return NULL;
	options = add_blksize_option(options, CLUSTER_SIZE(*ef.sb));
	if (options == NULL)
		return NULL;
#endif
	return options;
}

static char* add_passthrough_fuse_options(char* fuse_options,
		const char* options)
{
	const char* passthrough_list[] =
	{
#if defined(__FreeBSD__)
		"automounted",
#endif
		"nonempty",
		NULL
	};
	int i;

	for (i = 0; passthrough_list[i] != NULL; i++)
		if (exfat_match_option(options, passthrough_list[i]))
		{
			fuse_options = add_option(fuse_options, passthrough_list[i], NULL);
			if (fuse_options == NULL)
				return NULL;
		}

	return fuse_options;
}

static int fuse_exfat_main(char* mount_options, char* mount_point)
{
	char* argv[] = {"exfat", "-s", "-o", mount_options, mount_point, NULL};
	return fuse_main(sizeof(argv) / sizeof(argv[0]) - 1, argv,
			&fuse_exfat_ops, NULL);
}

int main(int argc, char* argv[])
{
	const char* spec = NULL;
	char* mount_point = NULL;
	char* fuse_options;
	char* exfat_options;
	int opt;
	int rc;

	printf("FUSE exfat %s\n", VERSION);

	fuse_options = strdup("allow_other,"
#if defined(__linux__) || defined(__FreeBSD__)
			"big_writes,"
#endif
#if defined(__linux__)
			"blkdev,"
#endif
			"default_permissions");
	exfat_options = strdup("ro_fallback");
	if (fuse_options == NULL || exfat_options == NULL)
	{
		free(fuse_options);
		free(exfat_options);
		exfat_error("failed to allocate options string");
		return 1;
	}

	while ((opt = getopt(argc, argv, "dno:Vv")) != -1)
	{
		switch (opt)
		{
		case 'd':
			fuse_options = add_option(fuse_options, "debug", NULL);
			if (fuse_options == NULL)
			{
				free(exfat_options);
				return 1;
			}
			break;
		case 'n':
			break;
		case 'o':
			exfat_options = add_option(exfat_options, optarg, NULL);
			if (exfat_options == NULL)
			{
				free(fuse_options);
				return 1;
			}
			fuse_options = add_passthrough_fuse_options(fuse_options, optarg);
			if (fuse_options == NULL)
			{
				free(exfat_options);
				return 1;
			}
			break;
		case 'V':
			free(exfat_options);
			free(fuse_options);
			puts("Copyright (C) 2010-2018  Andrew Nayenko");
			return 0;
		case 'v':
			break;
		default:
			free(exfat_options);
			free(fuse_options);
			usage(argv[0]);
			break;
		}
	}
	if (argc - optind != 2)
	{
		free(exfat_options);
		free(fuse_options);
		usage(argv[0]);
	}
	spec = argv[optind];
	mount_point = argv[optind + 1];

	if (exfat_mount(&ef, spec, exfat_options) != 0)
	{
		free(exfat_options);
		free(fuse_options);
		return 1;
	}

	free(exfat_options);

	fuse_options = add_fuse_options(fuse_options, spec, ef.ro != 0);
	if (fuse_options == NULL)
	{
		exfat_unmount(&ef);
		return 1;
	}

	/* let FUSE do all its wizardry */
	rc = fuse_exfat_main(fuse_options, mount_point);

	free(fuse_options);
	return rc;
}
