#include <assert.h>
#include <string.h>
#include <malloc.h>

#include "global.h"
#include "support.h"
#include "mirrors.h"
#include "zero-install.h"
#include "xml.h"

/* Decide the URI where the archive is to be downloaded from.
 * file is the cache-relative path of a file in the group.
 * free() the result.
 */
char *mirrors_get_best_url(const char *site, const char *leafname)
{
	char *uri = NULL;
	Element *mirror, *mirrors = NULL;
	char *path = NULL;
	const char *base;

	assert(strchr(site, '/') == NULL);
	assert(leafname);

	path = build_string("%s/%s/" META "/mirrors.xml", cache_dir, site);
	if (!path)
		return NULL;
	mirrors = xml_new(ZERO_NS, path);
	if (!mirrors) {
		error("Can't open '%s' file for site", path);
		goto out;
	}
	free(path);
	path = NULL;

	for (mirror = mirrors->lastChild; mirror;
					mirror = mirror->previousSibling) {
		break;
	}

	if (!mirror) {
		error("No mirrors found!");
		goto out;
	}

	base = xml_get_attr(mirror, "base");
	if (!base) {
		error("Missing 'base' attribute in mirrors.xml");
		goto out;
	}

	uri = build_string("%s/%s", base, leafname);
	if (!uri)
		goto out;

out:
	if (mirrors)
		xml_destroy(mirrors);
	if (path)
		free(path);

	return uri;
}
