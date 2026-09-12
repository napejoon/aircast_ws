/* Update check — notify only.
 *
 * This module never downloads an installer and never executes anything. It
 * fetches one small JSON manifest plus its detached minisign signature, checks
 * the signature against a public key compiled into this binary, and produces
 * three integers and a boolean. The "Update" button then opens a GitHub tag URL
 * built from those integers, in the user's browser.
 *
 * The reason for the signature, given that nothing is executed: the transport
 * only proves we talked to GitHub. Anyone who steals the Actions token or the
 * maintainer's account can publish a release — and would then be able to make
 * this program tell its users to go and install something. The signing key
 * lives offline and never enters CI, so that nudge is the one thing in this
 * design a repository compromise cannot forge.
 *
 * Full threat model, including what this does NOT protect: docs/threat-model.md
 */

#ifndef AIRCAST_UPDATE_CHECK_H
#define AIRCAST_UPDATE_CHECK_H

#include <gio/gio.h>
#include <glib.h>

typedef struct {
  /* FALSE until a real signing key is compiled in; the check then never runs
   * and says so, rather than silently looking healthy. */
  gboolean configured;
  gboolean available;

  /* Parsed from the signed manifest. The URL is built from these, never taken
   * from the network. */
  guint major, minor, patch;
  gchar *tag_url;
  gchar *asset;
  gchar *sha256;

  /* One line for the passive status area. Always set. */
  gchar *status;
  gint64 last_success;          /* unix seconds, 0 if never */
  gchar *high_water;            /* highest version ever verified, or NULL */
} AircastUpdate;

void aircast_update_free (AircastUpdate *update);

/* "1.2.3" or "v1.2.3" -> 1002003. Anything else, including a trailing
 * character, is -1: the version is the entire rollback defence, so a field it
 * cannot parse exactly must never compare as newer. */
gint64 aircast_ver_num (const char *s);

/* 0 only if sig is a minisign prehashed ("ED") signature over
 * BLAKE2b-512(manifest) by the compiled-in key. Non-zero otherwise, and the
 * caller must then not parse the manifest at all. */
int aircast_manifest_verify (const char *manifest, gsize mlen,
                             const char *sig, gsize slen);

gboolean aircast_update_key_configured (void);

/* user_initiated bypasses the once-per-day throttle. Runs on a worker thread;
 * the callback lands on the caller's main context. */
void aircast_update_check_async (const char *current_version,
                                 gboolean user_initiated,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data);
AircastUpdate *aircast_update_check_finish (GAsyncResult *result);

/* Both return 0 on success, for use as a process exit code. */
int aircast_update_selftest (void);
int aircast_update_selftest_manifest (const char *manifest_path, const char *sig_path);

#endif /* AIRCAST_UPDATE_CHECK_H */
