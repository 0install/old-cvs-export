/*
 * Zero Install -- user space helper
 *
 * Copyright (C) 2003  Thomas Leonard
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

/* This is a simple XML DOM-type interface to libexpat.
 * libxml is too big, and we already link to libexpat via dbus.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <expat.h>
#include <string.h>

#include "global.h"
#include "support.h"
#include "xml.h"

typedef struct _ParserState ParserState;

struct _Element {
	char *name;
	Element *lastChild;
	Element *previousSibling;
	Element *parentNode;
	char **attrs;
};

struct _ParserState {
	const char *namespaceURI;
	int namespaceURI_len;
	Element *root, *current;
	int waiting_for_ends;
	int oom;
};

static void free_attrs(char **attrs)
{
	int i;
	assert(attrs);
	for (i = 0; attrs[i]; i++)
		free(attrs[i]);
	free(attrs);
}
	
static char **copy_attrs(const XML_Char **atts)
{
	int i, n = 0;
	char **new;

	assert(atts);
	
	while (atts[n])
		n += 2;

	assert(n > 0);

	new = my_malloc(sizeof(char *) * (n + 1));
	if (!new)
		return NULL;

	for (i = 0; i < n; i++) {
		assert(atts[i]);

		new[i] = my_strdup(atts[i]);
		if (!new[i])
			goto oom;
	}

	new[n] = NULL;

	return new;
oom:
	free_attrs(new);
	return NULL;
}

static void free_element(Element *element)
{
	assert(element);

	if (element->name)
		free(element->name);
	if (element->attrs)
		free_attrs(element->attrs);
	free(element);
}

static void start_element(void *userData, const XML_Char *name,
			  const XML_Char **atts)
{
	ParserState *state = userData;
	Element *new = NULL;

	if (state->oom)
		return;

	if (state->waiting_for_ends) {
		state->waiting_for_ends++;
		return;
	}

	if (strncmp(state->namespaceURI, name, state->namespaceURI_len) != 0 ||
	    name[state->namespaceURI_len] != ' ') {
		/* Not our namespace; not interested */
		state->waiting_for_ends++;
		return;
	}
	
	new = my_malloc(sizeof(Element));
	if (!new)
		goto oom;

	new->parentNode = NULL;
	new->previousSibling = NULL;
	new->lastChild = NULL;
	new->attrs = NULL;

	new->name = my_strdup(name + state->namespaceURI_len + 1);
	if (!new->name)
		goto oom;

	if (atts && atts[0]) {
		new->attrs = copy_attrs(atts);
		if (!new->attrs)
			goto oom;
	}

	if (!state->root) {
		/* New root node */
		state->root = new;
	} else {
		/* Link into existing tree */
		new->parentNode = state->current;
		new->previousSibling = state->current->lastChild;
		state->current->lastChild = new;
	}

	state->current = new;
		
	return;
	
oom:
	if (new)
		free_element(new);
	state->oom = 1;
	return;
}

static void end_element(void *userData, const XML_Char *name)
{
	ParserState *state = userData;
	
	if (state->oom)
		return;
	if (state->waiting_for_ends) {
		state->waiting_for_ends--;
		return;
	}

	state->current = state->current->parentNode;
}

/* Returns the root object.
 * NULL on OOM.
 */
Element *xml_new(const char *namespace, const char *pathname)
{
	XML_Parser parser = NULL;
	ParserState state;
	int src = -1;

	assert(namespace);
	assert(pathname);

	state.root = state.current = NULL;
	state.namespaceURI = namespace;
	state.namespaceURI_len = strlen(namespace);
	state.waiting_for_ends = 0;
	state.oom = 0;

	src = open(pathname, O_RDONLY);
	if (src == -1) {
		perror("open");
		return NULL;
	}

	parser = XML_ParserCreateNS(NULL, ' ');
	if (!parser)
		goto err;
	
	XML_SetUserData(parser, &state);
	XML_SetStartElementHandler(parser, start_element);
	XML_SetEndElementHandler(parser, end_element);

	for (;;) {
		char buffer[256];
		int got;
		
		got = read(src, buffer, sizeof(buffer));
		if (got < 0) {
			perror("read");
			goto err;
		}
		if (!XML_Parse(parser, buffer, got, got == 0)) {
			fprintf(stderr, "Invalid XML\n");
			goto err;
		}
		if (state.oom)
			goto err;
		if (got == 0) {
			if (!state.root) {
				fprintf(stderr, "Bad root namespace\n");
				goto err;
			}
			goto out;
		}
	}

err:
	if (state.root) {
		xml_destroy(state.root);
		state.root = NULL;
	}

out:
	if (parser)
		XML_ParserFree(parser);
	if (src != -1)
		close(src);

	return state.root;
}

void xml_destroy(Element *root)
{
	Element *current = root;
	
	assert(root);

	while (current) {
		Element *next;

		if (current->lastChild) {
			current = current->lastChild;
			continue;
		}
		
		if (current->previousSibling)
			next = current->previousSibling;
		else {
			next = current->parentNode;
			if (next)
				next->lastChild = NULL;
		}

		free_element(current);
		current = next;
	}
}
