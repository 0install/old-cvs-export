#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <expat.h>
#include <string.h>

#include "child.h"
#include "zero-install.h"

#define ZERO_NS "http://zero-install.sf.net"

/* We only parse one document at a time, so... */
static int depth;
static int broken;
static int write_name;
static int wrote_name;
static FILE *ddd;

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
	if (broken)
		return;
	depth++;

	if (strncmp(name, ZERO_NS " ", sizeof(ZERO_NS)) != 0)
		goto err;
	name += sizeof(ZERO_NS);
	
	if (depth == 1) {
		if (strcmp(name, "directory") != 0)
			goto err;
	} else if (depth == 2) {
		if (strcmp(name, "dir") == 0) {
			fprintf(ddd, "d %ld %ld ",
				get_attr(atts, "size"),
				get_attr(atts, "mtime"));

			write_name = 1;
			wrote_name = 0;
		}
	}

	return;
err:
	broken = 1;
	return;
}

static void end_element(void *userData, const XML_Char *name)
{
	if (broken)
		return;
	depth--;
	if (write_name) {
		if (!wrote_name) {
			broken = 1;
		} else {
			write_name = 0;
			fputc('\0', ddd);
		}
	}
}

static void char_data(void *userData, const XML_Char *s, int len)
{
	int i;
	if (broken || !write_name || len < 1)
		return;
	for (i = 0; i < len; i++) {
		if (s[i] == '/' || s[i] == '\0') {
			broken = 1;
			return;
		}
	}
	if (fwrite(s, 1, len, ddd) != len)
		broken = 1;
	wrote_name = 1;
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
