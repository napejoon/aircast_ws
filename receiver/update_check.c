/* See update_check.h for what this is and, more importantly, what it is not. */

#include "update_check.h"

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <sodium.h>
#include <stdio.h>
#include <string.h>

/* Compile-time literals, all of them. A URL that arrives over the network is
 * an execute primitive waiting for a typo, so the manifest carries no URL
 * field and this program builds every address it ever opens. */
#define MANIFEST_URL \
  "https://github.com/napejoon/aircast_ws/releases/latest/download/aircast-update.json"
#define SIGNATURE_URL MANIFEST_URL ".minisig"
#define TAG_URL_FMT "https://github.com/napejoon/aircast_ws/releases/tag/v%u.%u.%u"
#define PRODUCT "aircast-receiver"

#define MANIFEST_MAX 8192
#define SIGNATURE_MAX 4096
#define IO_TIMEOUT_S 10
#define WALL_TIMEOUT_S 20
#define THROTTLE_S (24 * 60 * 60)
#define STALE_DAYS 30

/* The 32 public-key bytes of `aircast-update.pub`: base64-decode its second
 * line to 42 bytes ("Ed" ‖ key_id[8] ‖ pk[32]) and take the last 32.
 *
 * All zeroes means no key has been generated yet. The check then does not run
 * at all — it does not fetch, and it says so in the UI. A build that silently
 * accepts unsigned manifests would be worse than no update check, so this is
 * the one place the absence of a key must be loud. */
static const unsigned char AIRCAST_UPDATE_PK[32] = { 0 };

gboolean
aircast_update_key_configured (void)
{
  static const unsigned char zero[32] = { 0 };
  return memcmp (AIRCAST_UPDATE_PK, zero, sizeof zero) != 0;
}

/* ------------------------------------------------------------------ version */

gint64
aircast_ver_num (const char *s)
{
  guint a, b, c;
  char tail;

  if (!s || strlen (s) > 16)
    return -1;
  if (*s == 'v' || *s == 'V')
    s++;
  /* The %c is the point: sscanf returns 4 when any trailing byte exists, so
   * "1.2.3-rc1" and "1.2.3; calc.exe" are rejected rather than truncated. */
  if (sscanf (s, "%u.%u.%u%c", &a, &b, &c, &tail) != 3)
    return -1;
  if (a > 999 || b > 999 || c > 999)
    return -1;
  return (gint64) a * 1000000 + (gint64) b * 1000 + c;
}

/* ---------------------------------------------------------------- signature */

/* minisign's container, with the parts we deliberately do not read:
 *
 *   line 0  untrusted comment      never read
 *   line 1  base64(  "ED" ‖ key_id[8] ‖ sig[64]  )
 *   line 2  trusted comment        never read
 *   line 3  global signature over sig ‖ trusted comment — not checked, because
 *           nothing it covers is used. Every field this program acts on lives
 *           inside the signed JSON body.
 *
 * Getting minisign wrong usually means trusting its comments; the fix is to
 * not make its container the trust boundary. */
static int
manifest_verify_with_key (const char *manifest, gsize mlen,
                          const char *sig, gsize slen,
                          const unsigned char pk[32])
{
  char line[4][512];
  unsigned char blob[74], hash[64];
  size_t pos = 0, blen = 0;

  if (mlen == 0 || mlen > MANIFEST_MAX || slen == 0 || slen > SIGNATURE_MAX)
    return -1;

  for (int i = 0; i < 4; i++) {
    size_t k = 0;
    while (pos < slen && sig[pos] != '\n') {
      if (k + 1 >= sizeof line[i])
        return -1;
      if (sig[pos] != '\r')
        line[i][k++] = sig[pos];
      pos++;
    }
    if (pos >= slen && i < 3)
      return -1;                /* truncated */
    line[i][k] = '\0';
    pos++;
  }

  if (strncmp (line[0], "untrusted comment: ", 19) != 0)
    return -1;

  if (sodium_base642bin (blob, sizeof blob, line[1], strlen (line[1]),
                         NULL, &blen, NULL, sodium_base64_VARIANT_ORIGINAL) != 0
      || blen != sizeof blob)
    return -1;
  /* "ED" is prehashed; legacy "Ed" signs the whole file and is refused rather
   * than supported, so a downgrade to it is not a thing an attacker can try. */
  if (memcmp (blob, "ED", 2) != 0)
    return -1;

  crypto_generichash (hash, sizeof hash,
                      (const unsigned char *) manifest, mlen, NULL, 0);
  return crypto_sign_verify_detached (blob + 10, hash, sizeof hash, pk);
}

int
aircast_manifest_verify (const char *manifest, gsize mlen,
                         const char *sig, gsize slen)
{
  if (!aircast_update_key_configured ())
    return -1;
  return manifest_verify_with_key (manifest, mlen, sig, slen, AIRCAST_UPDATE_PK);
}

/* ------------------------------------------------------------------- state */

/* Nothing here is security-relevant on its own: the compiled-in version is
 * always a floor, so a deleted or rewritten state file can only ever lose the
 * memory of a higher version, never lower the bar. */
static gchar *
state_path (void)
{
  return g_build_filename (g_get_user_config_dir (), "aircast", "update.ini", NULL);
}

static GKeyFile *
state_load (void)
{
  GKeyFile *kf = g_key_file_new ();
  gchar *path = state_path ();
  g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, NULL);
  g_free (path);
  return kf;
}

static void
state_save (GKeyFile *kf)
{
  gchar *path = state_path ();
  gchar *dir = g_path_get_dirname (path);
  g_mkdir_with_parents (dir, 0700);
  g_key_file_save_to_file (kf, path, NULL);
  g_free (dir);
  g_free (path);
}

void
aircast_update_free (AircastUpdate *update)
{
  if (!update)
    return;
  g_free (update->tag_url);
  g_free (update->asset);
  g_free (update->sha256);
  g_free (update->status);
  g_free (update->high_water);
  g_free (update);
}

/* ------------------------------------------------------------------- fetch */

typedef struct {
  gchar *current_version;
  gboolean user_initiated;
} CheckRequest;

static void
check_request_free (gpointer data)
{
  CheckRequest *req = data;
  g_free (req->current_version);
  g_free (req);
}

static GBytes *
fetch (SoupSession *session, const char *url, gsize cap, GCancellable *cancellable)
{
  SoupMessage *msg = soup_message_new (SOUP_METHOD_GET, url);
  GError *error = NULL;

  if (!msg)
    return NULL;
  soup_message_headers_append (soup_message_get_request_headers (msg),
                               "Accept", "application/octet-stream");

  GBytes *body = soup_session_send_and_read (session, msg, cancellable, &error);
  if (!body) {
    g_clear_error (&error);
    g_object_unref (msg);
    return NULL;
  }

  /* Redirects are followed because GitHub's asset URL crosses hosts, so the
   * scheme has to be re-checked at the end: libsoup will follow https -> http. */
  GUri *final_uri = soup_message_get_uri (msg);
  gboolean ok = soup_message_get_status (msg) == SOUP_STATUS_OK
      && final_uri && g_strcmp0 (g_uri_get_scheme (final_uri), "https") == 0
      && g_bytes_get_size (body) > 0
      && g_bytes_get_size (body) < cap;

  g_object_unref (msg);
  if (!ok) {
    g_bytes_unref (body);
    return NULL;
  }
  return body;
}

/* Every field is validated before it is used, and the strings that reach the
 * UI are matched against a character class first: this label is the one place
 * the program shows text to a user who is deciding whether to install
 * something, so nothing shaped like a sentence ever comes out of the feed. */
static gboolean
field_ok (const char *s, const char *allowed, gsize max)
{
  if (!s || !*s || strlen (s) > max)
    return FALSE;
  for (const char *p = s; *p; p++)
    if (!strchr (allowed, *p))
      return FALSE;
  return TRUE;
}

static void
check_thread (GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
  CheckRequest *req = task_data;
  AircastUpdate *update = g_new0 (AircastUpdate, 1);
  GKeyFile *kf = state_load ();
  gint64 now = g_get_real_time () / G_USEC_PER_SEC;

  update->configured = TRUE;
  update->last_success = g_key_file_get_int64 (kf, "update", "last_success", NULL);
  update->high_water = g_key_file_get_string (kf, "update", "high_water", NULL);

  /* The throttle is keyed on attempts, not successes, so a hostile network
   * cannot make the app spin. A human pressing the button is exempt. */
  gint64 last_attempt = g_key_file_get_int64 (kf, "update", "last_attempt", NULL);
  if (!req->user_initiated && now - last_attempt < THROTTLE_S) {
    update->status = g_strdup ("Update check: waiting for the daily check");
    goto out;
  }
  g_key_file_set_int64 (kf, "update", "last_attempt", now);
  state_save (kf);

  SoupSession *session = soup_session_new ();
  soup_session_set_timeout (session, IO_TIMEOUT_S);
  soup_session_set_user_agent (session, "aircast-receiver/" AIRCAST_VERSION " ");

  GBytes *manifest = fetch (session, MANIFEST_URL, MANIFEST_MAX, cancellable);
  GBytes *signature = manifest
      ? fetch (session, SIGNATURE_URL, SIGNATURE_MAX, cancellable) : NULL;
  g_object_unref (session);

  if (!manifest || !signature) {
    g_clear_pointer (&manifest, g_bytes_unref);
    update->status = g_strdup ("Update check could not reach GitHub");
    goto out;
  }

  gsize mlen = 0, slen = 0;
  const char *mdata = g_bytes_get_data (manifest, &mlen);
  const char *sdata = g_bytes_get_data (signature, &slen);

  /* Verification comes before parsing: unverified bytes never reach the JSON
   * parser, so a parser bug is not reachable by anyone without the key. */
  if (aircast_manifest_verify (mdata, mlen, sdata, slen) != 0) {
    g_bytes_unref (manifest);
    g_bytes_unref (signature);
    update->status = g_strdup ("Update check: signature did not verify");
    goto out;
  }

  JsonParser *parser = json_parser_new ();
  gboolean parsed = json_parser_load_from_data (parser, mdata, mlen, NULL);
  JsonObject *obj = parsed && JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser))
      ? json_node_get_object (json_parser_get_root (parser)) : NULL;
  g_bytes_unref (manifest);
  g_bytes_unref (signature);

  if (!obj) {
    g_object_unref (parser);
    update->status = g_strdup ("Update check: manifest was not readable");
    goto out;
  }

  const char *version = json_object_get_string_member_with_default (obj, "version", "");
  const char *asset = json_object_get_string_member_with_default (obj, "asset", "");
  const char *sha256 = json_object_get_string_member_with_default (obj, "sha256", "");
  const char *product = json_object_get_string_member_with_default (obj, "product", "");
  gint64 schema = json_object_get_int_member_with_default (obj, "schema", 0);
  gint64 candidate = aircast_ver_num (version);

  gboolean well_formed = schema == 1
      && g_strcmp0 (product, PRODUCT) == 0
      && candidate > 0
      && field_ok (asset, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-", 80)
      && field_ok (sha256, "0123456789abcdef", 64) && strlen (sha256) == 64;

  if (!well_formed) {
    g_object_unref (parser);
    /* Signed but wrong is not a successful check: last_success is not advanced,
     * so a manifest that can never satisfy us still shows up as staleness. */
    update->status = g_strdup ("Update check: manifest failed validation");
    goto out;
  }

  /* The floor is the higher of what is installed and the highest version ever
   * verified, so replaying an older correctly-signed manifest cannot walk a
   * user backwards — and the attempt is reported rather than swallowed. */
  gint64 installed = aircast_ver_num (req->current_version);
  gint64 seen = update->high_water ? aircast_ver_num (update->high_water) : -1;
  gint64 floor = MAX (installed, seen);

  if (candidate <= floor) {
    if (candidate > installed) {
      update->status = g_strdup_printf (
          "Update check: refused an older update manifest (highest seen: v%s)",
          update->high_water ? update->high_water : "?");
    } else {
      update->status = g_strdup ("Up to date");
      g_key_file_set_int64 (kf, "update", "last_success", now);
      update->last_success = now;
      state_save (kf);
    }
    g_object_unref (parser);
    goto out;
  }

  guint a = 0, b = 0, c = 0;
  sscanf (version[0] == 'v' ? version + 1 : version, "%u.%u.%u", &a, &b, &c);
  update->available = TRUE;
  update->major = a;
  update->minor = b;
  update->patch = c;
  /* Built from the three validated integers. Never a string from the feed, and
   * never /releases/latest — the user lands on the exact tag we verified. */
  update->tag_url = g_strdup_printf (TAG_URL_FMT, a, b, c);
  update->asset = g_strdup (asset);
  update->sha256 = g_strdup (sha256);
  update->status = g_strdup_printf ("Version %u.%u.%u is available", a, b, c);

  g_free (update->high_water);
  update->high_water = g_strdup_printf ("%u.%u.%u", a, b, c);
  g_key_file_set_string (kf, "update", "high_water", update->high_water);
  g_key_file_set_int64 (kf, "update", "last_success", now);
  update->last_success = now;
  state_save (kf);
  g_object_unref (parser);

out:
  g_key_file_unref (kf);
  g_task_return_pointer (task, update, (GDestroyNotify) aircast_update_free);
}

static gboolean
cancel_on_wall_clock (gpointer data)
{
  g_cancellable_cancel (G_CANCELLABLE (data));
  return G_SOURCE_REMOVE;
}

void
aircast_update_check_async (const char *current_version,
                            gboolean user_initiated,
                            GAsyncReadyCallback callback,
                            gpointer user_data)
{
  GCancellable *cancellable = g_cancellable_new ();
  GTask *task = g_task_new (NULL, cancellable, callback, user_data);

  if (!aircast_update_key_configured ()) {
    AircastUpdate *update = g_new0 (AircastUpdate, 1);
    update->configured = FALSE;
    /* No key, no fetch. Saying this out loud is the point: a check that looks
     * healthy while verifying nothing is the failure this design exists to
     * avoid. */
    update->status = g_strdup ("Update checks are not configured in this build");
    g_task_return_pointer (task, update, (GDestroyNotify) aircast_update_free);
    g_object_unref (task);
    g_object_unref (cancellable);
    return;
  }

  CheckRequest *req = g_new0 (CheckRequest, 1);
  req->current_version = g_strdup (current_version);
  req->user_initiated = user_initiated;
  g_task_set_task_data (task, req, check_request_free);

  /* soup_session_set_timeout is per-I/O, so a peer that drips one byte at a
   * time resets it forever. This bounds the whole attempt. */
  g_timeout_add_seconds_full (G_PRIORITY_DEFAULT, WALL_TIMEOUT_S,
                              cancel_on_wall_clock,
                              g_object_ref (cancellable), g_object_unref);

  g_task_run_in_thread (task, check_thread);
  g_object_unref (task);
  g_object_unref (cancellable);
}

AircastUpdate *
aircast_update_check_finish (GAsyncResult *result)
{
  AircastUpdate *update = g_task_propagate_pointer (G_TASK (result), NULL);
  if (update)
    return update;

  update = g_new0 (AircastUpdate, 1);
  update->configured = TRUE;
  update->status = g_strdup ("Update check failed");
  return update;
}

/* ----------------------------------------------------------------- selftest */

/* Builds a real minisign "ED" signature with a throwaway key, so the verifier
 * is exercised rather than described. Returns a newly allocated .minisig. */
static gchar *
make_sig (const char *manifest, gsize mlen, const unsigned char sk[64],
          const char *algo)
{
  unsigned char hash[64], sig[64], blob[74];
  char b64[256];

  crypto_generichash (hash, sizeof hash, (const unsigned char *) manifest, mlen, NULL, 0);
  crypto_sign_detached (sig, NULL, hash, sizeof hash, sk);
  memcpy (blob, algo, 2);
  memset (blob + 2, 0x41, 8);           /* key id: never read by the verifier */
  memcpy (blob + 10, sig, sizeof sig);
  sodium_bin2base64 (b64, sizeof b64, blob, sizeof blob, sodium_base64_VARIANT_ORIGINAL);

  return g_strdup_printf ("untrusted comment: test\n%s\ntrusted comment: test\n%s\n",
                          b64, b64);
}

int
aircast_update_selftest (void)
{
  if (sodium_init () < 0)
    return 1;

  /* The comparison is the whole rollback defence. */
  g_assert (aircast_ver_num ("1.10.0") > aircast_ver_num ("1.9.0"));
  g_assert (aircast_ver_num ("2.0.0") > aircast_ver_num ("1.99.99"));
  g_assert (aircast_ver_num ("v1.2.3") == aircast_ver_num ("1.2.3"));
  g_assert (aircast_ver_num ("1.2.3-rc1") == -1);
  g_assert (aircast_ver_num ("1.2.3; calc.exe") == -1);
  g_assert (aircast_ver_num ("99999.0.0") == -1);
  g_assert (aircast_ver_num ("1.2") == -1);
  g_assert (aircast_ver_num (NULL) == -1);

  unsigned char pk[32], sk[64];
  crypto_sign_keypair (pk, sk);

  const char *manifest =
      "{\"schema\":1,\"product\":\"aircast-receiver\",\"version\":\"1.4.0\"}";
  gsize mlen = strlen (manifest);
  gchar *sig = make_sig (manifest, mlen, sk, "ED");
  gsize slen = strlen (sig);

  g_assert (manifest_verify_with_key (manifest, mlen, sig, slen, pk) == 0);

  /* One flipped manifest byte. */
  gchar *tampered = g_strdup (manifest);
  tampered[10] ^= 0x01;
  g_assert (manifest_verify_with_key (tampered, mlen, sig, slen, pk) != 0);
  g_free (tampered);

  /* One flipped signature byte. The offset matters: base64 characters 0..13
   * cover the "ED" tag and the key id, and the key id is deliberately never
   * read, so a flip there legitimately still verifies. Character 30 is inside
   * the 64 signature bytes. */
  gchar *bad_sig = g_strdup (sig);
  gchar *second_line = strchr (bad_sig, '\n') + 1;
  g_assert (strlen (second_line) > 40);
  second_line[30] = second_line[30] == 'A' ? 'B' : 'A';
  g_assert (manifest_verify_with_key (manifest, mlen, bad_sig, strlen (bad_sig), pk) != 0);
  g_free (bad_sig);

  /* And the property that flip exposed, asserted on purpose: the key id is not
   * part of what is verified. If this ever starts failing, someone has started
   * trusting a field the signature does not cover. */
  gchar *other_id = g_strdup (sig);
  gchar *id_line = strchr (other_id, '\n') + 1;
  id_line[5] = id_line[5] == 'A' ? 'B' : 'A';
  g_assert (manifest_verify_with_key (manifest, mlen, other_id, strlen (other_id), pk) == 0);
  g_free (other_id);

  /* Truncated. */
  g_assert (manifest_verify_with_key (manifest, mlen, sig, 20, pk) != 0);

  /* Legacy non-prehashed tag is refused outright rather than supported. */
  gchar *legacy = make_sig (manifest, mlen, sk, "Ed");
  g_assert (manifest_verify_with_key (manifest, mlen, legacy, strlen (legacy), pk) != 0);
  g_free (legacy);

  /* A mutated trusted comment still verifies — correct, because nothing ever
   * reads it. If this ever starts failing, someone has made minisign's
   * container the trust boundary again. */
  gchar *mutated = g_strdup (sig);
  gchar *comment = strstr (mutated, "trusted comment: test");
  comment[17] = 'X';
  g_assert (manifest_verify_with_key (manifest, mlen, mutated, strlen (mutated), pk) == 0);
  g_free (mutated);

  /* A different key must not verify. */
  unsigned char other_pk[32], other_sk[64];
  crypto_sign_keypair (other_pk, other_sk);
  g_assert (manifest_verify_with_key (manifest, mlen, sig, slen, other_pk) != 0);

  g_free (sig);
  return 0;
}

int
aircast_update_selftest_manifest (const char *manifest_path, const char *sig_path)
{
  gchar *manifest = NULL, *sig = NULL;
  gsize mlen = 0, slen = 0;

  if (sodium_init () < 0)
    return 1;
  if (!aircast_update_key_configured ()) {
    g_printerr ("no signing key is compiled into this build\n");
    return 2;
  }
  if (!g_file_get_contents (manifest_path, &manifest, &mlen, NULL)
      || !g_file_get_contents (sig_path, &sig, &slen, NULL)) {
    g_printerr ("cannot read the manifest or its signature\n");
    g_free (manifest);
    g_free (sig);
    return 3;
  }

  int rc = aircast_manifest_verify (manifest, mlen, sig, slen);
  g_print (rc == 0 ? "signature OK\n" : "SIGNATURE DID NOT VERIFY\n");
  g_free (manifest);
  g_free (sig);
  return rc == 0 ? 0 : 4;
}
