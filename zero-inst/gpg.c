#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "support.h"
#include "gpg.h"

/* Implements the GPG signature checking. Someone who understands GPG
 * fully should really check this stuff over...
 */

#define GPG_OPTIONS "--quiet --no-tty --batch --homedir . 2>/dev/null"

#define MATCH(x) (strncmp(buffer, x, sizeof(x) - 1) == 0)

static int ok_for_site(const char *email, const char *site)
{
	if (strncmp(email, "<0install@", sizeof("<0install@") - 1) != 0)
		return 0;
	email += sizeof("<0install@") - 1;

	if (strncmp(email, site, strlen(site)) != 0)
		return 0;

	email += strlen(site);

	return email[0] == '>' && email[1] == '\n';
}

/* Check that ./index.new is signed by ./index.xml.sig, whose public key
 * is known to us.
 * Merge keyring.pub into our database of known keys, and check there is
 * a trust path to it.
 * If we have no keys yet, trust everything in keyring.pub!
 * 1 if index.new looks OK, 0 to reject it.
 */
int gpg_trusted(const char *site)
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
		error("Failed to merge new keys!");
		return 0;
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
					return 0;	/* OOM */
			} else
				error("Old key corrupted! "
					"Skipping security check!");
		}

		have_trusted_key = trusted_key != NULL;

		if (trusted_key) {
			command = build_string("gpg " GPG_OPTIONS
				" --status-fd 1"
				" --trusted-key %s"
				" --verify index.xml.sig index.new",
				trusted_key);

			free(trusted_key);
		} else {
			command = build_string("gpg " GPG_OPTIONS
				" --status-fd 1"
				" --verify index.xml.sig index.new");
		}
	}

	out = popen(command, "r");
	free(command);
	if (!out) {
		error("popen: %m");
		return 0;
	}

	current_key[0] = '\0';
	current_key[16] = '\0';

	while (1) {
		char buffer[4096];

		if (!fgets(buffer, sizeof(buffer), out))
			break;

		if (MATCH("[GNUPG:] GOODSIG ")) {
			const char *langle;
			langle = strrchr(buffer, '<');
			if (langle && ok_for_site(langle, site)) {
				assert(buffer[17 + 16] == ' ');
				memcpy(current_key, buffer + 17, 16);
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
				return 0;
			}

			fprintf(new, "%s\n", current_key);
			fclose(new);
		}
	}

	pclose(out);

	if (!trusted) {
		error("New index is NOT signed with a key with "
			"a trust path from the old key!");
		return 0;
	}
	
	if (have_trusted_key)
		error("New index is signed OK -- trusting");
	else 
		error("Blindly trusting key for new site");

	return 1;
}

