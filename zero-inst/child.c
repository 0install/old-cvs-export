#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <expat.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include "child.h"
#include "zero-install.h"

#define ZERO_NS "http://zero-install.sf.net"

/* We only parse one document at a time, so... */
static int depth;
static int broken;
static int write_name;
static FILE *ddd;

static char item_in_group;
static char found_type;
static const char *current_leaf;

static char *current_archive;
static int current_archive_len;	/* Buffer size */

#define MAX_CONTENT_LEN 4096
static int content_len = -1;	/* Set to 0 to start recording */
static char content[MAX_CONTENT_LEN];

static long get_attr(const XML_Char **atts, const char *name)
{
	while (*atts) {
		if (strcmp(name, atts[0]) == 0)
			return atol(atts[1]);
		atts += 2;
	}

	broken = 1;
	return 0;
}

static void start_element(void *userData, const XML_Char *name,
			  const XML_Char **atts)
{
	content_len = -1;

	if (broken)
		return;
	depth++;

	if (strncmp(name, ZERO_NS " ", sizeof(ZERO_NS)) != 0)
		return;
	name += sizeof(ZERO_NS);
	
	if (depth == 1) {
		if (strcmp(name, "directory") != 0)
			goto err;
	} else if (depth == 2) {
		if (strcmp(name, "dir") == 0) {
			fprintf(ddd, "%c %ld %ld ",
				name[0] == 'd' ? 'd' : 'l',
				get_attr(atts, "size"),
				get_attr(atts, "mtime"));

			content_len = 0;
			write_name = 1;
		}
	} else if (depth == 3) {
		if (strcmp(name, "file") == 0 ||
		    strcmp(name, "exec") == 0) {
			fprintf(ddd, "%c %ld %ld ",
				name[0] == 'f' ? 'f' : 'x',
				get_attr(atts, "size"),
				get_attr(atts, "mtime"));

			content_len = 0;
			write_name = 1;
		}
	}

	return;
err:
	broken = 1;
	return;
}

static void end_element(void *userData, const XML_Char *name)
{
	depth--;
	if (broken || !write_name)
		goto out;
	if (content_len <= 0) {
		broken = 1;
	} else {
		write_name = 0;
		content[content_len] = '\0';
		content_len++;
		if (strchr(content, '/') || strlen(content) != content_len - 1)
			broken = 1;
		else if (fwrite(content, 1, content_len, ddd) != content_len)
			broken = 1;
	}
out:
	content_len = -1;
}

static void char_data(void *userData, const XML_Char *s, int len)
{
	if (content_len == -1 || len < 1)
		return;
	if (content_len + len < sizeof(content) - 1) {
		memcpy(content + content_len, s, len);
		content_len += len;
	} else
		content_len = -1;
}

void build_ddd_from_index(const char *dir)
{
	XML_Parser parser = NULL;
	int index = -1;

	if (chdir(dir)) {
		perror("chdir");
		return;
	}
	
	index = open(ZERO_INST_INDEX, O_RDONLY);
	if (index == -1)
		goto err;

	parser = XML_ParserCreateNS(NULL, ' ');
	if (!parser) {
		fprintf(stderr, "Failed to create XML parser");
		goto out;
	}
	XML_SetStartElementHandler(parser, start_element);
	XML_SetEndElementHandler(parser, end_element);
	XML_SetCharacterDataHandler(parser, char_data);

	depth = 0;
	broken = 0;
	content_len = -1;
	write_name = 0;

	ddd = fopen("....", "w");
	if (!ddd)
		goto err;
	fprintf(ddd, "LazyFS\n");

	for (;;) {
		char buffer[256];
		int got;
		
		got = read(index, buffer, sizeof(buffer));
		if (got < 0)
			goto err;
		if (!XML_Parse(parser, buffer, got, got == 0) || broken) {
			fprintf(stderr, "Invalid XML\n");
			goto out;
		}
		if (got == 0)
			break;
	}

	if (fclose(ddd))
		goto err;
	if (rename("....", "..."))
		goto err;
	goto out;
err:
	perror("build_ddd_from_index");
out:
	if (index != -1)
		close(index);
	if (parser)
		XML_ParserFree(parser);
	chdir("/");
}

static void start_element_find(void *userData, const XML_Char *name,
				const XML_Char **atts)
{
	content_len = -1;

	if (broken || found_type)
		return;
	if (strncmp(name, ZERO_NS " ", sizeof(ZERO_NS)) != 0)
		return;
	name += sizeof(ZERO_NS);

	if (strcmp(name, "group") == 0) {
		item_in_group = '\0';
		current_archive[0] = '\0';
	} else if (strcmp(name, "dir") == 0 ||
		   strcmp(name, "file") == 0 ||
		   strcmp(name, "link") == 0 ||
		   strcmp(name, "exec") == 0 ||
		   strcmp(name, "archive") == 0)
		content_len = 0;
}

static void end_element_find(void *userData, const XML_Char *name)
{
	if (broken || found_type)
		return;
	if (strcmp(name, ZERO_NS " group") == 0) {
		if (item_in_group)
			found_type = item_in_group;
		return;
	}
	if (content_len == -1)
		return;
	content[content_len] = '\0';

	if (strcmp(name, ZERO_NS " archive") == 0) {
		content_len++;
		if (content_len <= current_archive_len)
			memcpy(current_archive, content, content_len);
		printf("[ got archive '%s' ]\n", current_archive);
	}
	else if (strcmp(content, current_leaf) == 0) {
		printf("Match in %s!\n", name);
		item_in_group = name[sizeof(ZERO_NS)];
	}
	content_len = -1;
}

/* Return the URI of the archive containing 'leaf' by reading the given
 * XML index file.
 * Returns EISDIR if it's a directory, 0 on success or EINVAL for other
 * errors.
 */
int get_item_info(const char *index_path, const char *leaf, char *uri, int len)
{
	XML_Parser parser = NULL;
	int err = EINVAL;
	int index = -1;

	assert(len > 0);

	printf("[ lookup '%s' ]\n", leaf);

	index = open(index_path, O_RDONLY);
	if (index == -1) {
		perror("open");
		goto out;
	}

	parser = XML_ParserCreateNS(NULL, ' ');
	if (!parser) {
		fprintf(stderr, "Failed to create XML parser");
		goto out;
	}

	XML_SetStartElementHandler(parser, start_element_find);
	XML_SetEndElementHandler(parser, end_element_find);
	XML_SetCharacterDataHandler(parser, char_data);

	broken = 0;
	content_len = -1;
	found_type = '\0';
	item_in_group = '\0';
	current_leaf = leaf;
	current_archive = uri;
	current_archive_len = len;
	*uri = '\0';

	for (;;) {
		char buffer[256];
		int got;
		
		got = read(index, buffer, sizeof(buffer));
		if (got < 0) {
			perror("read");
			err = EINVAL;
			goto out;
		}
		if (!XML_Parse(parser, buffer, got, got == 0) || broken) {
			fprintf(stderr, "Invalid XML\n");
			err = EINVAL;
			goto out;
		}
		if (found_type || got == 0)
			break;
	}

	if (!found_type) {
		fprintf(stderr, "Item '%s' not found in '%s'\n",
				leaf, index_path);
		return EINVAL;
	} else if (!*uri) {
		if (found_type == 'd')
			return EISDIR;
		fprintf(stderr, "No archive for '%s'\n", leaf);
		return EINVAL;
	}

	err = 0;
out:
	if (index != -1)
		close(index);
	if (parser)
		XML_ParserFree(parser);

	return err;
}
