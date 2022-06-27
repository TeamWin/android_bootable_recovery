/*
	main.c (02.09.09)
	exFAT file system checker.

	Free exFAT implementation.
	Copyright (C) 2011-2018  Andrew Nayenko

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
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>

#define exfat_debug(format, ...) do {} while (0)

uint64_t files_count, directories_count;

static int nodeck(struct exfat* ef, struct exfat_node* node)
{
	const cluster_t cluster_size = CLUSTER_SIZE(*ef->sb);
	cluster_t clusters = DIV_ROUND_UP(node->size, cluster_size);
	cluster_t c = node->start_cluster;
	int rc = 0;

	while (clusters--)
	{
		if (CLUSTER_INVALID(*ef->sb, c))
		{
			char name[EXFAT_UTF8_NAME_BUFFER_MAX];

			exfat_get_name(node, name);
			exfat_error("file '%s' has invalid cluster 0x%x", name, c);
			rc = 1;
			break;
		}
		if (BMAP_GET(ef->cmap.chunk, c - EXFAT_FIRST_DATA_CLUSTER) == 0)
		{
			char name[EXFAT_UTF8_NAME_BUFFER_MAX];

			exfat_get_name(node, name);
			exfat_error("cluster 0x%x of file '%s' is not allocated", c, name);
			rc = 1;
		}
		c = exfat_next_cluster(ef, node, c);
	}
	return rc;
}

static void dirck(struct exfat* ef, const char* path)
{
	struct exfat_node* parent;
	struct exfat_node* node;
	struct exfat_iterator it;
	int rc;
	size_t path_length;
	char* entry_path;

	if (exfat_lookup(ef, &parent, path) != 0)
		exfat_bug("directory '%s' is not found", path);
	if (!(parent->attrib & EXFAT_ATTRIB_DIR))
		exfat_bug("'%s' is not a directory (%#hx)", path, parent->attrib);
	if (nodeck(ef, parent) != 0)
	{
		exfat_put_node(ef, parent);
		return;
	}

	path_length = strlen(path);
	entry_path = malloc(path_length + 1 + EXFAT_UTF8_NAME_BUFFER_MAX);
	if (entry_path == NULL)
	{
		exfat_put_node(ef, parent);
		exfat_error("out of memory");
		return;
	}
	strcpy(entry_path, path);
	strcat(entry_path, "/");

	rc = exfat_opendir(ef, parent, &it);
	if (rc != 0)
	{
		free(entry_path);
		exfat_put_node(ef, parent);
		return;
	}
	while ((node = exfat_readdir(&it)))
	{
		exfat_get_name(node, entry_path + path_length + 1);
		exfat_debug("%s: %s, %"PRIu64" bytes, cluster %u", entry_path,
				node->is_contiguous ? "contiguous" : "fragmented",
				node->size, node->start_cluster);
		if (node->attrib & EXFAT_ATTRIB_DIR)
		{
			directories_count++;
			dirck(ef, entry_path);
		}
		else
		{
			files_count++;
			nodeck(ef, node);
		}
		exfat_flush_node(ef, node);
		exfat_put_node(ef, node);
	}
	exfat_closedir(ef, &it);
	exfat_flush_node(ef, parent);
	exfat_put_node(ef, parent);
	free(entry_path);
}

static void fsck(struct exfat* ef, const char* spec, const char* options)
{
	if (exfat_mount(ef, spec, options) != 0)
	{
		fputs("File system checking stopped. ", stdout);
		return;
	}

	exfat_print_info(ef->sb, exfat_count_free_clusters(ef));
	exfat_soil_super_block(ef);
	dirck(ef, "");
	exfat_unmount(ef);

	printf("Totally %"PRIu64" directories and %"PRIu64" files.\n",
			directories_count, files_count);
	fputs("File system checking finished. ", stdout);
}

static void usage(const char* prog)
{
	fprintf(stderr, "Usage: %s [-a | -n | -p | -y] <device>\n", prog);
	fprintf(stderr, "       %s -V\n", prog);
	exit(1);
}

int main(int argc, char* argv[])
{
	int opt;
	const char* options;
	const char* spec = NULL;
	struct exfat ef;

	printf("exfatfsck %s\n", VERSION);

	if (isatty(STDIN_FILENO))
		options = "repair=1";
	else
		options = "repair=0";

	while ((opt = getopt(argc, argv, "anpVy")) != -1)
	{
		switch (opt)
		{
		case 'a':
		case 'p':
		case 'y':
			options = "repair=2";
			break;
		case 'n':
			options = "repair=0,ro";
			break;
		case 'V':
			puts("Copyright (C) 2011-2018  Andrew Nayenko");
			return 0;
		default:
			usage(argv[0]);
			break;
		}
	}
	if (argc - optind != 1)
		usage(argv[0]);
	spec = argv[optind];

	printf("Checking file system on %s.\n", spec);
	fsck(&ef, spec, options);
	if (exfat_errors != 0)
	{
		printf("ERRORS FOUND: %d, FIXED: %d.\n",
				exfat_errors, exfat_errors_fixed);
		return 1;
	}
	puts("No errors found.");
	return 0;
}
