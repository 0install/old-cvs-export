#include <ctype.h>

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "global.h"
#include "support.h"
#include "gpg.h"

/* Implements the GPG signature checking. Someone who understands GPG
 * fully should really check this stuff over...
 */

#define GPG_OPTIONS "--quiet --no-tty --batch --homedir . 2>/dev/null"

#define MATCH(x) (strncmp(buffer, x, sizeof(x) - 1) == 0)

static int ok_for_site(const char *name, const char *site)
{
	int domain_len;
	char *hash;
	const char *email;

	email = strchr(name, '<');
	if (!email) {
		error("Signature name has no email address!");
		return 0;
	}

	hash = strchr(site, '#');
	if (hash) {
		domain_len = hash - site;
		if (strncmp(email, "<0sub@", sizeof("<0sub@") - 1) != 0)
			return 0;
		email += sizeof("<0sub@") - 1;
	} else {
		/* For root indexes, only care about the email part
		 * (for backwards-compatibility).
		 */
		domain_len = strlen(site);
		if (strncmp(email, "<0install@", sizeof("<0install@") - 1) != 0)
			return 0;
		email += sizeof("<0install@") - 1;
	}
	
	if (strncmp(email, site, domain_len) != 0)
		return 0;
	email += domain_len;
	site += domain_len;

	return *email == '>';	/* XXX: space or newline next? */
}

/* Check that ./<leafname> is signed by ./index.xml.sig, whose public key
 * is known to us. <leafname> must not contain funny characters.
 * Merge keyring.pub into our database of known keys, and check there is
 * a trust path to it.
 * If we have no keys yet, trust everything in keyring.pub!
 * NULL if <leafname> looks OK, otherwise returns an error message.
 */
const char *gpg_trusted(const char *site, const char *leafname)
{
	/* The key used to sign the last accepted version of the index.
	 * We ultimately trust this key to sign others. The key used to sign
	 * the new index becomes the trusted_key next time, if there is
	 * a trust path to it.
	 * If we have no trusted_key, we blindly trust whatever key is used.
	 */
	char *command;
	FILE *out;
	int trusted = 0;
	char current_key[17];
	int have_trusted_key = 0;

	/* TODO: escape it somehow */
	assert(strchr(site, '\'') == NULL);
	assert(strchr(site, '\\') == NULL);
	
	if (system("gpg " GPG_OPTIONS " --import keyring.pub")) {
		return "Failed to merge new keys! Is GPG installed?";
	}

	/* Try to get the key we used last time */
	{
		FILE *old_key;
		char *trusted_key = NULL;

		old_key = fopen("trusted_key", "r");
		if (old_key) {
			current_key[16] = '\0';
			fread(current_key, 1, 17, old_key);
			fclose(old_key);
			if (current_key[16] == '\n') {
				/* Managed to read the key OK */
				current_key[16] = '\0';
				trusted_key = my_strdup(current_key);
				if (!trusted_key)
					return "Out of memory";
			} else
				error("Old key corrupted! "
					"Skipping security check!");
		}

		have_trusted_key = trusted_key != NULL;

		if (trusted_key) {
			command = build_string("gpg " GPG_OPTIONS
				" --status-fd 1"
				" --trusted-key %s"
				" --verify index.xml.sig '%s'",
				trusted_key, leafname);

			free(trusted_key);
		} else {
			command = build_string("gpg " GPG_OPTIONS
				" --status-fd 1"
				" --verify index.xml.sig '%s'",
				leafname);
		}
	}

	out = popen(command, "r");
	free(command);
	if (!out) {
		error("popen: %m");
		return "Failed to pipe through GPG (check error log)";
	}

	current_key[0] = '\0';
	current_key[16] = '\0';

	while (1) {
		char buffer[4096];

		if (!fgets(buffer, sizeof(buffer), out))
			break;

		if (MATCH("[GNUPG:] GOODSIG ")) {
			const char *key_text = buffer + 17;
			const char *space;
			space = strchr(key_text, ' ');
			if (space && ok_for_site(space + 1, site)) {
				assert(key_text[16] == ' ');
				memcpy(current_key, key_text, 16);
			} else
				current_key[0] = '\0';
		}

		if (!*current_key)
			continue;

		if (MATCH("[GNUPG:] TRUST_ULTIMATE\n") ||
		    MATCH("[GNUPG:] TRUST_FULLY\n") ||
		    MATCH("[GNUPG:] TRUST_MARGINAL\n") ||
		    (MATCH("[GNUPG:] TRUST_UNDEFINED\n") &&
		     		!have_trusted_key)) {
			FILE *new;

			trusted = 1;

			new = fopen("trusted_key", "w");
			if (!new) {
				error("fopen: %m");
				return "Failed to save new key "
					"(check error log)";
			}

			fprintf(new, "%s\n", current_key);
			fclose(new);
		}
	}

	pclose(out);

	if (!trusted) {
		if (have_trusted_key)
			return "New index is NOT signed with a key with "
				"a trust path from the old key!";
		else
			return "New index is not correctly signed!";
	}
	
	if (have_trusted_key)
		error("New index is signed OK -- trusting");
	else 
		error("Blindly trusting key for new site");

	return NULL;
}

