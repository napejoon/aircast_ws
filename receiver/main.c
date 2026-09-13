/* aircast receiver — GTK4 window around a GStreamer webrtcbin.
 *
 * Shape of the UI, which is deliberately Reflector-like: a dark room with
 * nothing in it but the pairing code until a phone connects, then the mirrored
 * screen inside a device bezel, with a toolbar that appears on mouse movement
 * and gets out of the way again.
 *
 * Media path (docs/research/stack-options.md §2): webrtcbin's pads are
 * application/x-rtp only, so the tail is built per pad from the caps —
 *
 *   webrtcbin. ! rtph264depay ! h264parse ! tee ! queue ! d3d11h264dec ! video/x-raw ! videoconvert ! gtk4paintablesink
 *                                             \ ! queue ! matroskamux ! filesink   (while recording)
 *
 * An explicit decoder, not decodebin, and d3d11h264dec only where the machine
 * has it: avdec_h264 stands in everywhere else. The bare video/x-raw after the
 * hardware decoder is not decoration. It is what makes the decoder read the
 * frame back to system memory rather than hand videoconvert a D3D11 texture it
 * cannot map, which was the black window 994401c blamed on the decoder itself.
 *
 * Recording is a tee branch off the depayloaded stream, added and removed at
 * runtime, so what lands on disk is the phone's own bitstream — no decode, no
 * re-encode, and no quality cost to watching it.
 *
 * Build: see CMakeLists.txt.
 */

#include <gtk/gtk.h>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <string.h>

#ifdef G_OS_WIN32
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#endif

#include "update_check.h"

#define TOOLBAR_HIDE_MS 2500

typedef struct {
  /* configuration */
  gchar *signal_url;
  gchar *code;
  gchar *record_dir;
  guint latency_ms;
  gboolean relay_only;
  gboolean insecure;
  gboolean selftest;
  gchar *verify_manifest;
  gchar *verify_signature;

  /* signalling */
  SoupSession *session;
  SoupMessage *message;
  SoupWebsocketConnection *ws;

  /* media */
  GstElement *pipeline;
  GstElement *webrtc;
  GstElement *tee;              /* owned by the tail bin */
  GstElement *record_branch;
  GstPad *record_tee_pad;
  gchar *record_mux;            /* the muxer half of the tail, per codec */
  gchar *record_file;

  /* interface */
  GtkApplication *app;
  GtkWidget *window;
  GtkWidget *stack;
  GtkWidget *picture;
  GtkWidget *code_label;
  GtkWidget *status_label;
  GtkWidget *revealer;
  GtkWidget *record_button;
  GtkWidget *record_time;
  GtkWidget *update_label;      /* passive: last checked, highest version seen */
  gchar *update_url;            /* built from verified integers, or NULL */

  guint hide_source;
  guint record_timer;
  gint64 record_started;
  gboolean recording;
} App;

/* Run before anything else in main(), and specifically before
 * g_option_context_parse(), because gst_init() runs inside it and the registry
 * scan is the largest LoadLibrary surface in the process.
 *
 * GLib's own guard against module-path variables is g_check_setuid(), which
 * returns FALSE on Windows — so anything that can write HKCU\Environment gets
 * arbitrary DLLs loaded into this process, with no admin and no writable
 * install directory. No signature scheme anywhere else in this program touches
 * that, which is why this is the first thing that happens. */
static void
harden_environment (void)
{
#ifdef G_OS_WIN32
  /* Windows only, and deliberately. On Linux these variables are the user's own
   * — GST_PLUGIN_PATH is how receiver/README.md says to reach the gtk4
   * paintable sink that apt does not ship, and clearing it made the program
   * report that plugin missing on a machine where it is installed. Nothing is
   * defended by clearing them there: anyone who can set this process's
   * environment can run code as this user anyway. HKCU\Environment is the
   * asymmetry, and it only exists on Windows.
   */
  g_unsetenv ("GIO_EXTRA_MODULES");
  g_unsetenv ("GIO_USE_TLS");
  g_unsetenv ("GST_PLUGIN_PATH");
  g_unsetenv ("GST_PLUGIN_PATH_1_0");
  g_unsetenv ("GST_PLUGIN_SYSTEM_PATH");
  g_unsetenv ("GST_PLUGIN_SYSTEM_PATH_1_0");
  g_unsetenv ("GST_REGISTRY");

  /* The Windows build ships as a relocatable bundle: bin/ holds the exe and
   * every DLL, and each layer finds its data by walking up from the DLL it was
   * loaded from. XDG_DATA_DIRS defeats that — when it is set and non-empty,
   * GLib never appends <root>/share, the bundled gschemas.compiled goes
   * invisible, and g_settings_new() aborts. Anyone launching from Git Bash or
   * an MSYS2 shell has it set. */
  g_unsetenv ("XDG_DATA_DIRS");

  gchar *root = g_win32_get_package_installation_directory_of_module (NULL);
  if (root) {
    gchar *schemas = g_build_filename (root, "share", "glib-2.0", "schemas", NULL);
    g_setenv ("GSETTINGS_SCHEMA_DIR", schemas, TRUE);
    g_free (schemas);
    g_free (root);
  }

  /* Scope, stated honestly: this governs later LoadLibraryEx calls only. GTK,
   * GStreamer, GLib, json-glib and libsoup are static imports resolved by the
   * loader before main() runs, under the standard search order. DEFAULT_DIRS
   * keeps the application directory — GStreamer's plugins live there — which is
   * only sound because the installer puts us in %ProgramFiles%. That installer
   * choice, not this call, is what makes the application directory safe. */
  SetDefaultDllDirectories (LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  SetDllDirectoryW (L"");
#endif
}

static void on_pad_added (GstElement *webrtc, GstPad *pad, App *self);
static void send_json (App *self, JsonBuilder *builder);
static void set_status (App *self, const gchar *text);
static void stop_recording (App *self);

/* ----------------------------------------------------------------- interface */

static void
show_page (App *self, const gchar *page)
{
  gtk_stack_set_visible_child_name (GTK_STACK (self->stack), page);
}

static void
set_status (App *self, const gchar *text)
{
  gtk_label_set_text (GTK_LABEL (self->status_label), text);
}

/* GStreamer callbacks arrive on streaming threads; GTK may only be touched
 * from the main one. Everything the pipeline wants to say to the UI goes
 * through one of these. */
typedef struct {
  App *self;
  gchar *text;
  gchar *page;
} UiUpdate;

static gboolean
apply_ui_update (gpointer data)
{
  UiUpdate *update = data;
  if (update->text)
    set_status (update->self, update->text);
  if (update->page)
    show_page (update->self, update->page);
  g_free (update->text);
  g_free (update->page);
  g_free (update);
  return G_SOURCE_REMOVE;
}

static void
post_ui (App *self, const gchar *text, const gchar *page)
{
  UiUpdate *update = g_new0 (UiUpdate, 1);
  update->self = self;
  update->text = g_strdup (text);
  update->page = g_strdup (page);
  g_idle_add (apply_ui_update, update);
}

static gboolean
hide_toolbar (gpointer data)
{
  App *self = data;
  self->hide_source = 0;
  /* Never hide the toolbar while recording: the red dot lives in it, and a
   * recording nobody can see is a recording nobody stops. */
  if (!self->recording)
    gtk_revealer_set_reveal_child (GTK_REVEALER (self->revealer), FALSE);
  return G_SOURCE_REMOVE;
}

static void
wake_toolbar (App *self)
{
  gtk_revealer_set_reveal_child (GTK_REVEALER (self->revealer), TRUE);
  if (self->hide_source)
    g_source_remove (self->hide_source);
  self->hide_source = g_timeout_add (TOOLBAR_HIDE_MS, hide_toolbar, self);
}

static void
on_motion (GtkEventControllerMotion *controller, gdouble x, gdouble y, App *self)
{
  wake_toolbar (self);
}

static gboolean
tick_record_time (gpointer data)
{
  App *self = data;
  gint64 seconds = (g_get_monotonic_time () - self->record_started) / G_USEC_PER_SEC;
  gchar *text = g_strdup_printf ("%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT,
      seconds / 60, seconds % 60);
  gtk_label_set_text (GTK_LABEL (self->record_time), text);
  g_free (text);
  return G_SOURCE_CONTINUE;
}

/* -------------------------------------------------------------------- record */

/* The branch is built and torn down while the pipeline plays, so the user can
 * start recording in the middle of a session — which is the only time anyone
 * ever decides to. */
static void
start_recording (App *self)
{
  if (self->recording || !self->tee || !self->record_mux)
    return;

  gchar *stamp = g_date_time_format_iso8601 (g_date_time_new_now_local ());
  g_strdelimit (stamp, ":", '-');
  g_free (self->record_file);
  self->record_file = g_build_filename (
      self->record_dir ? self->record_dir : g_get_home_dir (),
      g_strdup_printf ("aircast-%s.mkv", stamp), NULL);
  g_free (stamp);

  gchar *escaped = g_strescape (self->record_file, NULL);
  gchar *desc = g_strdup_printf (
      "queue max-size-time=0 max-size-bytes=0 ! %s ! filesink location=\"%s\"",
      self->record_mux, escaped);
  g_free (escaped);

  GError *error = NULL;
  self->record_branch = gst_parse_bin_from_description (desc, TRUE, &error);
  g_free (desc);
  if (error) {
    set_status (self, error->message);
    g_error_free (error);
    return;
  }

  gst_bin_add (GST_BIN (self->pipeline), self->record_branch);
  gst_element_sync_state_with_parent (self->record_branch);

  self->record_tee_pad = gst_element_request_pad_simple (self->tee, "src_%u");
  GstPad *sink = gst_element_get_static_pad (self->record_branch, "sink");
  gst_pad_link (self->record_tee_pad, sink);
  gst_object_unref (sink);

  self->recording = TRUE;
  self->record_started = g_get_monotonic_time ();
  self->record_timer = g_timeout_add_seconds (1, tick_record_time, self);
  gtk_widget_add_css_class (self->record_button, "recording");
  gtk_widget_set_visible (self->record_time, TRUE);
  wake_toolbar (self);

  gchar *msg = g_strdup_printf ("Recording to %s", self->record_file);
  set_status (self, msg);
  g_free (msg);
}

static gboolean
drop_record_branch (gpointer data)
{
  App *self = data;

  if (self->record_branch) {
    gst_element_set_state (self->record_branch, GST_STATE_NULL);
    gst_bin_remove (GST_BIN (self->pipeline), self->record_branch);
    self->record_branch = NULL;
  }
  if (self->record_tee_pad) {
    gst_element_release_request_pad (self->tee, self->record_tee_pad);
    gst_object_unref (self->record_tee_pad);
    self->record_tee_pad = NULL;
  }
  gchar *msg = g_strdup_printf ("Saved %s", self->record_file ? self->record_file : "");
  set_status (self, msg);
  g_free (msg);
  return G_SOURCE_REMOVE;
}

static GstPadProbeReturn
unlink_record_branch (GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
  App *self = data;
  GstPad *sink = gst_element_get_static_pad (self->record_branch, "sink");

  gst_pad_unlink (pad, sink);
  /* An EOS is what makes matroskamux write its index and close the file. */
  gst_pad_send_event (sink, gst_event_new_eos ());
  gst_object_unref (sink);

  /* ponytail: fixed 700 ms for the muxer to finish instead of waiting for the
   * branch's own EOS message. Swap in a bus watch on the branch if a long
   * recording ever comes out truncated. */
  g_timeout_add (700, drop_record_branch, self);
  return GST_PAD_PROBE_REMOVE;
}

static void
stop_recording (App *self)
{
  if (!self->recording)
    return;
  self->recording = FALSE;

  if (self->record_timer) {
    g_source_remove (self->record_timer);
    self->record_timer = 0;
  }
  gtk_widget_remove_css_class (self->record_button, "recording");
  gtk_widget_set_visible (self->record_time, FALSE);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->record_button), FALSE);

  if (self->record_tee_pad)
    gst_pad_add_probe (self->record_tee_pad, GST_PAD_PROBE_TYPE_IDLE,
        unlink_record_branch, self, NULL);
}

static void
on_record_toggled (GtkToggleButton *button, App *self)
{
  if (gtk_toggle_button_get_active (button)) {
    if (!self->tee) {
      gtk_toggle_button_set_active (button, FALSE);
      set_status (self, "Nothing to record yet");
      return;
    }
    start_recording (self);
  } else {
    stop_recording (self);
  }
}

static void
on_fullscreen_clicked (GtkButton *button, App *self)
{
  GtkWindow *window = GTK_WINDOW (self->window);
  if (gtk_window_is_fullscreen (window))
    gtk_window_unfullscreen (window);
  else
    gtk_window_fullscreen (window);
}

static void
on_disconnect_clicked (GtkButton *button, App *self)
{
  gtk_window_close (GTK_WINDOW (self->window));
}

static gboolean
on_key_pressed (GtkEventControllerKey *controller, guint keyval, guint code,
    GdkModifierType state, App *self)
{
  switch (keyval) {
    case GDK_KEY_f:
    case GDK_KEY_F11:
      on_fullscreen_clicked (NULL, self);
      return TRUE;
    case GDK_KEY_r:
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->record_button), !self->recording);
      return TRUE;
    case GDK_KEY_Escape:
      if (gtk_window_is_fullscreen (GTK_WINDOW (self->window))) {
        gtk_window_unfullscreen (GTK_WINDOW (self->window));
        return TRUE;
      }
      return FALSE;
    default:
      return FALSE;
  }
}

/* ---------------------------------------------------------------- signalling */

/* webrtcbin wants URIs shaped "turn(s)://user:pass@host:port", while the server
 * sends a urls array plus separate credentials. Splice the credentials into
 * every usable entry, percent-encoding them — a minted username is
 * "<expiry>:<id>" and that colon would otherwise end the userinfo early.
 *
 * Every entry, not just the first. The "turn-server" property holds exactly
 * one URI and the server lists the UDP entry first, so taking the first left
 * the receiver with no fallback at all on a network that drops outbound UDP —
 * and relay-only ICE means one stranded leg is zero sessions, however well the
 * phone connected. webrtcbin's own documentation says to use the
 * add-turn-server action signal when there is more than one. */
static GStrv
turn_uris (JsonObject *turn)
{
  /* A server that omits any of these is not trusted to crash us. */
  if (!json_object_has_member (turn, "urls") ||
      !json_object_has_member (turn, "username") ||
      !json_object_has_member (turn, "credential"))
    return NULL;

  JsonArray *urls = json_object_get_array_member (turn, "urls");
  const gchar *user = json_object_get_string_member (turn, "username");
  const gchar *pass = json_object_get_string_member (turn, "credential");
  GPtrArray *out;

  if (!urls || !user || !pass)
    return NULL;

  out = g_ptr_array_new ();
  for (guint i = 0; i < json_array_get_length (urls); i++) {
    const gchar *url = json_array_get_string_element (urls, i);
    const gchar *scheme = NULL;

    if (g_str_has_prefix (url, "turns:"))
      scheme = "turns";
    else if (g_str_has_prefix (url, "turn:"))
      scheme = "turn";
    else
      continue;                 /* stun: entries carry no credentials */

    const gchar *host = strchr (url, ':') + 1;
    gchar *escaped_user = g_uri_escape_string (user, NULL, FALSE);
    gchar *escaped_pass = g_uri_escape_string (pass, NULL, FALSE);
    /* Drop any ?transport= suffix: webrtcbin parses it as part of the port. */
    gchar **parts = g_strsplit (host, "?", 2);

    g_ptr_array_add (out, g_strdup_printf ("%s://%s:%s@%s",
        scheme, escaped_user, escaped_pass, parts[0]));
    g_strfreev (parts);
    g_free (escaped_user);
    g_free (escaped_pass);
  }

  if (out->len == 0) {
    g_ptr_array_free (out, TRUE);
    return NULL;
  }
  g_ptr_array_add (out, NULL);
  return (GStrv) g_ptr_array_free (out, FALSE);
}

/* The mid of the m-line a candidate belongs to, read out of our own local
 * description. docs/protocol/signalling.md says a candidate carries both
 * sdpMid and sdpMLineIndex; we were sending only the index, and the sender
 * passes whatever arrives straight into RTCIceCandidate. Returns NULL when the
 * description is not there yet, in which case the member is simply omitted —
 * the index alone is a valid candidate. Copied, because the attribute belongs
 * to the description this function frees. */
static gchar *
candidate_mid (GstElement *webrtc, guint mline)
{
  GstWebRTCSessionDescription *local = NULL;
  gchar *mid = NULL;

  g_object_get (webrtc, "local-description", &local, NULL);
  if (local && local->sdp && mline < gst_sdp_message_medias_len (local->sdp)) {
    const GstSDPMedia *media = gst_sdp_message_get_media (local->sdp, mline);
    mid = g_strdup (gst_sdp_media_get_attribute_val (media, "mid"));
  }
  if (local)
    gst_webrtc_session_description_free (local);
  return mid;
}

static void
on_ice_candidate (GstElement *webrtc, guint mline, gchar *candidate, App *self)
{
  gchar *mid = candidate_mid (webrtc, mline);
  JsonBuilder *b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "type");
  json_builder_add_string_value (b, "candidate");
  json_builder_set_member_name (b, "candidate");
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "candidate");
  json_builder_add_string_value (b, candidate);
  if (mid) {
    json_builder_set_member_name (b, "sdpMid");
    json_builder_add_string_value (b, mid);
  }
  json_builder_set_member_name (b, "sdpMLineIndex");
  json_builder_add_int_value (b, mline);
  json_builder_end_object (b);
  json_builder_end_object (b);
  g_free (mid);
  send_json (self, b);
}

/* Relay-only ICE either works or fails; without this the window would sit on
 * the idle card forever when the relay is unreachable. */
static void
on_connection_state (GstElement *webrtc, GParamSpec *pspec, App *self)
{
  GstWebRTCPeerConnectionState state;
  g_object_get (webrtc, "connection-state", &state, NULL);

  if (state == GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTED)
    post_ui (self, "Connected", NULL);
  else if (state == GST_WEBRTC_PEER_CONNECTION_STATE_FAILED)
    post_ui (self, "The connection failed — is the TURN relay reachable?", "idle");
}

/* The only place a pipeline failure has ever been able to say anything. An
 * element that fails after the pipeline is PLAYING posts to the bus and stops,
 * and until now nothing read the bus, so both black windows this receiver has
 * shipped looked identical from the outside: a healthy ICE connection and an
 * empty page. A decoder chosen at runtime needs this before the choice is worth
 * making, because the machine where the choice was wrong is otherwise
 * indistinguishable from the machine where the phone never sent a frame.
 * post_ui rather than set_status: the message arrives on whichever thread
 * posted it, and post_ui already hops to the main context. */
static gboolean
on_bus_message (GstBus *bus, GstMessage *msg, gpointer data)
{
  App *self = data;
  if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR) {
    GError *err = NULL;
    gchar *debug = NULL;
    gst_message_parse_error (msg, &err, &debug);
    /* A message can arrive with no source; naming it is the whole point of the
     * line, so fall back rather than dereference nothing. */
    GstObject *src = GST_MESSAGE_SRC (msg);
    gchar *text = g_strdup_printf ("%s failed: %s",
        src ? GST_OBJECT_NAME (src) : "The pipeline", err->message);
    post_ui (self, text, NULL);
    g_printerr ("%s\n%s\n", text, debug ? debug : "");
    g_free (text);
    g_free (debug);
    g_error_free (err);
  }
  return G_SOURCE_CONTINUE;
}

/* A mirrored screen arriving over a relay is heavily reordered: on one run
 * 8,234 of 18,065 packets came in behind a higher sequence number, and every
 * one of them was less than 20 ms late. rtpjitterbuffer drops a packet it has
 * already declared lost, and with retransmission off it declares one the
 * instant a gap appears instead of waiting — that run logged "Clearing gap
 * packets" on every single packet and threw 9,703 arrivals away. Each hole
 * costs a frame and the decoder then sits until the next keyframe, which is
 * exactly the stutter, and the freeze behind it.
 *
 * webrtcbin takes do-retransmission from the transceiver's do-nack, which the
 * handler below now turns on, so this is belt as well as braces. Keep it: what
 * it buys here is that the buffer waits out a reorder before calling it a loss,
 * and it stays right for a sender that offers no NACK at all.
 *
 * This handler runs after webrtcbin's own, so this is the value that sticks.
 *
 * rtx-next-seqnum is the one retransmission default that is wrong for a
 * mirrored screen. With it on, every arriving packet arms a retransmission
 * request for the packet the buffer estimates comes next, one packet spacing
 * later. A camera always sends that packet. A tablet whose screen has stopped
 * moving does not, so the request goes out for a packet that was never encoded.
 * In the last log some 2,300 requests, nine percent of every request the buffer
 * made, were for one past the highest sequence number received. Five of the
 * thirteen still-screen pauses hold exactly one, fired 38 to 68 ms after the
 * last real packet, and each of those timed out about 180 ms later into a
 * "Packet lost" event for a packet the phone had never sent.
 *
 * A lost event makes the buffer mark the next one discont, and rtph264depay
 * with request-keyframe=true answers a discont with a force-key-unit, so a
 * screen going still ends up ordering a full 2304x1440 keyframe that the phone
 * can only encode once the screen moves again. It then lands inside the burst
 * the movement itself produces, and that burst is where the loss already is:
 * the 10,508 lost packets of that log arrive in 307 runs averaging 34
 * consecutive packets, each run behind a peak of 650 to 830 packets a second,
 * which is the 6 Mbit ceiling the sender is set to. Guessing at a packet the
 * phone never sent is how a still screen gets charged for that burst twice.
 *
 * What turning the guess off gives up is the trailing packet of a frame: a hole
 * at a frame boundary is now only noticed when the next frame arrives, about
 * 17 ms later at the 60 fps the sender is now allowed. The same log measures
 * the round trip from request to retransmission at about 120 ms against a
 * 200 ms buffer, so that packet still comes back with room to spare. */
static void
on_new_jitterbuffer (GstElement *rtpbin, GstElement *jitterbuffer,
                     guint session, guint ssrc, App *self)
{
  g_object_set (jitterbuffer,
      "do-retransmission", TRUE,
      "rtx-next-seqnum", FALSE,
      NULL);
}

/* The phone is not the one withholding generic NACK. libwebrtc puts nack, nack
 * pli, ccm fir and transport-cc on every video codec it offers, and the codec
 * reordering the Flutter app does cannot take any of them away: it hands back
 * the codec it found in its own supported list, feedback and all. This side
 * throws it away. A transceiver webrtcbin invents for a remote m= section is
 * born with do-nack FALSE, and answering with that deletes the rtcp-fb-nack
 * field from the answer caps and skips the RTX payload type. So the answer says
 * no retransmission, thank you; the phone believes us and tears down the RTX
 * stream its own offer set up, the a=ssrc-group:FID that is plainly there in
 * the offer, and the 1,469 packets that genuinely went missing were never going
 * to come back, whatever the jitterbuffer above had been told to do. It is also
 * why the negotiated caps read nack-pli and ccm-fir but never nack: we edited
 * that line out of the answer ourselves.
 *
 * webrtcbin invents that transceiver while it works through the offer and emits
 * this signal before it reads do-nack to build the answer, which is the one
 * moment the flag still counts. */
static void
on_new_transceiver (GstElement *webrtc, GstWebRTCRTPTransceiver *trans,
                    App *self)
{
  g_object_set (trans, "do-nack", TRUE, NULL);
}

static gboolean
build_pipeline (App *self, JsonObject *turn)
{
  GStrv uris = turn_uris (turn);
  if (!uris && self->relay_only) {
    set_status (self, "The server sent no usable TURN URL");
    return FALSE;
  }

  self->pipeline = gst_pipeline_new ("aircast-receiver");
  GstBus *bus = gst_element_get_bus (self->pipeline);
  gst_bus_add_watch (bus, on_bus_message, self);
  gst_object_unref (bus);

  self->webrtc = gst_element_factory_make ("webrtcbin", "recv");
  if (!self->webrtc) {
    set_status (self, "webrtcbin is missing — install gstreamer1.0-plugins-bad");
    g_strfreev (uris);
    return FALSE;
  }

  g_object_set (self->webrtc,
      "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE,
      /* Direct first, relay when direct cannot be had. This was relay-only for
       * a long time, on the stated grounds that neither peer would learn the
       * other's address, and that turned out to be half true and expensive.
       *
       * Half true: libnice puts the local socket address in every relayed
       * candidate's raddr field (discovery.c sets base_addr from the socket it
       * allocated through, and _generate_candidate_sdp writes raddr whenever it
       * differs from the candidate address) and has no sanitiser. libwebrtc
       * does have one and empties that field under a relay-only filter. So the
       * phone was already being told this machine's address, and only the
       * phone's half of the promise was ever kept.
       *
       * Expensive: everything went through a relay in another country, which
       * is 47 ms of one-way path on its own, and a retransmission crossing it
       * twice is what the 200 ms jitter buffer is sized for. On one Wi-Fi that
       * whole structure covers a hop of a millisecond or two.
       *
       * ICE picks the fast path by itself. Type preference puts a host pair
       * orders of magnitude above a relay pair in both stacks, the relay
       * candidates are still gathered and still win on a network that blocks
       * peer-to-peer traffic, and neither trickle gathering nor the first
       * connection gets slower for having more candidates to try.
       *
       * --relay-only puts the old behaviour back for anyone who would rather
       * the other end saw nothing but the relay. The sender has to agree:
       * --dart-define=AIRCAST_RELAY=true is its half, and a mismatch is safe
       * but pointless, since the relay-only side offers no host candidate to
       * pair with and the session falls back to the relay either way. */
      "ice-transport-policy", self->relay_only
          ? GST_WEBRTC_ICE_TRANSPORT_POLICY_RELAY
          : GST_WEBRTC_ICE_TRANSPORT_POLICY_ALL,
      "latency", self->latency_ms,
      NULL);
  /* The jitterbuffers are created on the fly, one per stream, and webrtcbin
   * exposes no property for them — its rtpbin child and this signal are the
   * only way in. */
  GstElement *rtpbin = gst_bin_get_by_name (GST_BIN (self->webrtc), "rtpbin");
  if (rtpbin) {
    g_signal_connect (rtpbin, "new-jitterbuffer",
        G_CALLBACK (on_new_jitterbuffer), self);
    gst_object_unref (rtpbin);
  } else {
    g_warning ("webrtcbin has no rtpbin child: a reordered packet will be "
               "dropped as lost, and the picture will stutter");
  }

  /* add-turn-server rather than the turn-server property, so the UDP entry and
   * the TCP/TLS ones are all offered and ICE picks whichever the network
   * permits. It returns FALSE for a URI it could not parse; one bad entry is
   * not a reason to abandon the others, but a run where none of them took is
   * worth seeing in the log. */
  guint accepted = 0;
  for (guint i = 0; uris && uris[i]; i++) {
    gboolean ok = FALSE;
    g_signal_emit_by_name (self->webrtc, "add-turn-server", uris[i], &ok);
    if (ok)
      accepted++;
    else
      g_warning ("webrtcbin refused a TURN URI from the server");
  }
  if (uris && accepted == 0)
    g_warning ("no TURN URI was accepted; relay-only ICE will not connect");
  g_strfreev (uris);

  gst_bin_add (GST_BIN (self->pipeline), self->webrtc);
  g_signal_connect (self->webrtc, "pad-added", G_CALLBACK (on_pad_added), self);
  g_signal_connect (self->webrtc, "on-new-transceiver",
      G_CALLBACK (on_new_transceiver), self);
  g_signal_connect (self->webrtc, "on-ice-candidate", G_CALLBACK (on_ice_candidate), self);
  g_signal_connect (self->webrtc, "notify::connection-state",
      G_CALLBACK (on_connection_state), self);

  gst_element_set_state (self->pipeline, GST_STATE_PLAYING);
  return TRUE;
}

static void
send_json (App *self, JsonBuilder *builder)
{
  JsonGenerator *gen = json_generator_new ();
  JsonNode *root = json_builder_get_root (builder);
  json_generator_set_root (gen, root);
  gchar *text = json_generator_to_data (gen, NULL);

  if (self->ws)
    soup_websocket_connection_send_text (self->ws, text);

  g_free (text);
  json_node_free (root);
  g_object_unref (gen);
  g_object_unref (builder);
}

static void
send_answer (GstPromise *promise, gpointer user_data)
{
  App *self = user_data;
  const GstStructure *reply = gst_promise_get_reply (promise);
  GstWebRTCSessionDescription *answer = NULL;

  gst_structure_get (reply, "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, NULL);
  gst_promise_unref (promise);
  if (!answer) {
    post_ui (self, "Failed to create an answer", NULL);
    return;
  }

  g_signal_emit_by_name (self->webrtc, "set-local-description", answer, NULL);

  gchar *sdp = gst_sdp_message_as_text (answer->sdp);
  JsonBuilder *b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "type");
  json_builder_add_string_value (b, "answer");
  json_builder_set_member_name (b, "sdp");
  json_builder_add_string_value (b, sdp);
  json_builder_end_object (b);
  send_json (self, b);

  g_free (sdp);
  gst_webrtc_session_description_free (answer);
}

static void
on_offer (App *self, const gchar *sdp_text)
{
  GstSDPMessage *sdp = NULL;

  if (!sdp_text || gst_sdp_message_new_from_text (sdp_text, &sdp) != GST_SDP_OK) {
    set_status (self, "The sender's offer is not valid SDP");
    return;
  }

  GstWebRTCSessionDescription *offer =
      gst_webrtc_session_description_new (GST_WEBRTC_SDP_TYPE_OFFER, sdp);
  g_signal_emit_by_name (self->webrtc, "set-remote-description", offer, NULL);
  gst_webrtc_session_description_free (offer);

  set_status (self, "Negotiating…");
  g_signal_emit_by_name (self->webrtc, "create-answer", NULL,
      gst_promise_new_with_change_func (send_answer, self, NULL));
}

static void
on_message (SoupWebsocketConnection *ws, gint type, GBytes *bytes, App *self)
{
  gsize size = 0;
  const gchar *text = g_bytes_get_data (bytes, &size);
  JsonParser *parser = json_parser_new ();

  if (type != SOUP_WEBSOCKET_DATA_TEXT ||
      !json_parser_load_from_data (parser, text, size, NULL)) {
    g_object_unref (parser);
    return;
  }

  JsonObject *msg = json_node_get_object (json_parser_get_root (parser));
  const gchar *kind = json_object_get_string_member_with_default (msg, "type", "");

  if (g_str_equal (kind, "joined")) {
    build_pipeline (self, json_object_get_object_member (msg, "turn"));
  } else if (g_str_equal (kind, "peer")) {
    set_status (self, "A phone is connecting…");
  } else if (g_str_equal (kind, "offer")) {
    on_offer (self, json_object_get_string_member (msg, "sdp"));
  } else if (g_str_equal (kind, "candidate")) {
    JsonObject *c = json_object_get_object_member (msg, "candidate");
    g_signal_emit_by_name (self->webrtc, "add-ice-candidate",
        (guint) json_object_get_int_member_with_default (c, "sdpMLineIndex", 0),
        json_object_get_string_member (c, "candidate"));
  } else if (g_str_equal (kind, "bye")) {
    stop_recording (self);
    show_page (self, "idle");
    set_status (self, "The phone stopped casting");
  } else if (g_str_equal (kind, "error")) {
    /* Never render the server's own words: this label also carries the update
     * notice, and an attacker-controlled string in it is a phishing primitive. */
    g_message ("signalling error: %s",
        json_object_get_string_member_with_default (msg, "message", "(no detail)"));
    set_status (self, "Signalling error");
  }

  g_object_unref (parser);
}

static void
on_ws_closed (SoupWebsocketConnection *ws, App *self)
{
  set_status (self, "The signalling connection closed");
}

static void
on_connected (GObject *session, GAsyncResult *result, gpointer user_data)
{
  App *self = user_data;
  GError *error = NULL;

  self->ws = soup_session_websocket_connect_finish (SOUP_SESSION (session), result, &error);
  if (error) {
    gchar *msg = g_strdup_printf ("Cannot reach the signalling server: %s", error->message);
    set_status (self, msg);
    g_free (msg);
    g_error_free (error);
    return;
  }

  g_signal_connect (self->ws, "message", G_CALLBACK (on_message), self);
  g_signal_connect (self->ws, "closed", G_CALLBACK (on_ws_closed), self);

  JsonBuilder *b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "type");
  json_builder_add_string_value (b, "join");
  json_builder_set_member_name (b, "code");
  json_builder_add_string_value (b, self->code);
  json_builder_set_member_name (b, "role");
  json_builder_add_string_value (b, "receiver");
  json_builder_end_object (b);
  send_json (self, b);

  set_status (self, "Waiting for a phone");
}

/* ------------------------------------------------------------------ pipeline */

typedef struct {
  App *self;
  GstElement *sink;
} PaintableHandover;

/* Runs on the main thread (g_idle_add), which is the only place gtk4paintablesink
 * will hand out its paintable — the "paintable" property getter errors on any
 * other thread, and on_pad_added runs on a streaming one. Fetching it here
 * rather than there is the difference between a live picture and a blank one. */
static gboolean
attach_paintable (gpointer data)
{
  PaintableHandover *handover = data;
  App *self = handover->self;
  GdkPaintable *paintable = NULL;

  g_object_get (handover->sink, "paintable", &paintable, NULL);
  if (paintable) {
    gtk_picture_set_paintable (GTK_PICTURE (self->picture), paintable);
    g_object_unref (paintable);
    show_page (self, "live");
    wake_toolbar (self);
  } else {
    set_status (self, "The video sink produced no paintable");
  }

  gst_object_unref (handover->sink);
  g_free (handover);
  return G_SOURCE_REMOVE;
}

/* One tail per incoming pad, chosen from the RTP caps. H.264 is what the phone
 * sends when it can; VP8 is the floor for devices libwebrtc does not consider
 * H.264-capable, and the two tails differ only in the first two elements. */
static void
on_pad_added (GstElement *webrtc, GstPad *pad, App *self)
{
  if (GST_PAD_DIRECTION (pad) != GST_PAD_SRC)
    return;

  GstCaps *caps = gst_pad_get_current_caps (pad);
  if (!caps)
    caps = gst_pad_query_caps (pad, NULL);
  const GstStructure *s = gst_caps_get_structure (caps, 0);
  const gchar *encoding = gst_structure_get_string (s, "encoding-name");
  if (!encoding) {
    gst_caps_unref (caps);
    return;
  }

  const gchar *head = NULL;
  const gchar *mux = NULL;
  /* An explicit decoder per codec, not decodebin. Two reasons, both seen the
   * hard way: decodebin plugged d3d11h264dec and handed the plain videoconvert
   * downstream a D3D11 texture it cannot map, a decoder that plugs and a sink
   * that stays black; and decodebin will not plug anything at all until typefind
   * has seen a decodable frame, which never arrives until the keyframe request
   * below is wired. Only the first of those was ever about the decoder, and it
   * turned out not to be: what was missing was a capsfilter telling it to read
   * the frame back. So the hardware decoder is here again, named rather than
   * autoplugged, with that capsfilter attached. vp8dec is unchanged; it outputs
   * the system memory videoconvert always takes. */
  const gchar *dec = NULL;
  if (g_ascii_strcasecmp (encoding, "H264") == 0) {
    /* request-keyframe makes rtph264depay push an upstream force-key-unit,
     * which webrtcbin turns into an RTCP PLI. libwebrtc emits a keyframe only
     * at stream start or on a PLI, so a receiver that joins mid-stream — and
     * every ~30s ICE reconnect — otherwise never gets an SPS/IDR: h264parse
     * stays silent and nothing decodes, which is exactly the black window we
     * had. config-interval=-1 repeats SPS/PPS before each IDR so a
     * re-established flow describes itself without another round trip.
     *
     * wait-for-keyframe is off because it was the stutter. The jitterbuffer
     * marks the first buffer after a loss DISCONT; on a DISCONT the depayloader
     * empties its adapter and bins every access unit it finishes until an
     * SPS/PPS/IDR arrives whole. That left the picture frozen for 52 of 333
     * seconds on one run, a median of 0.9 s and once 7.3 s against a 52 ms
     * round trip, because the quarter-megabyte IDR each PLI asks for is dropped
     * by the relay as readily as the frame that started it, so the wait renews
     * itself. Over those freezes the jitterbuffer still handed out some 1,250
     * finished frames that never reached the screen, and roughly seven in ten
     * of them had not lost a packet: counted from the jitterbuffer's own output
     * inside the freeze windows, because the depayloader logs its drops only at
     * GST_LOG. avdec_h264 never logged a line all session, because it never saw
     * any of it. The black window does not come back: the PLI hangs off
     * request-keyframe alone, pushed from that same DISCONT path, and the first
     * buffer out of a fresh jitterbuffer is always DISCONT. Nothing here is
     * decodebin any more either, so the leading P-frames h264parse cannot
     * describe are dropped there instead. */
    head = "rtph264depay request-keyframe=true wait-for-keyframe=false "
           "! h264parse config-interval=-1";
    /* Hardware decode where the machine has it, software everywhere else. The
     * test is the factory's rank and not merely the factory, so
     * GST_PLUGIN_FEATURE_RANK=d3d11h264dec:none puts avdec back for an A/B
     * without a rebuild.
     *
     * The bare video/x-raw is load-bearing. d3d11h264dec's src template offers
     * video/x-raw(memory:D3D11Memory) first and plain video/x-raw second, and
     * caps carrying no features match system memory only, so the filter picks
     * the second and the decoder reads the frame back instead of handing
     * videoconvert a texture it cannot map.
     *
     * This buys CPU, not milliseconds, and the log says why. The sink paints
     * every frame at its PTS plus the pipeline's 215 ms, to the millisecond,
     * while the jitterbuffer hands the frame over a median 51 ms past that PTS.
     * The 164 ms of clock wait in between swallows whatever the decoder costs,
     * so neither decoder is on the critical path until that 215 ms comes down.
     * Neither holds a frame back either: the phone sends constrained baseline
     * and the jitterbuffer answers the latency query live, so GstH264Decoder
     * zeroes its reorder delay and GstDxvaH264Decoder adds no output delay of
     * its own.
     *
     * What does change is the damaged frame. avdec's output-corrupt=false
     * dropped the one frame a loss had wrecked; DXVA has no equivalent, and the
     * base class's discard-corrupted-frames waits on a flag nothing on this
     * path ever sets, so that frame is painted with the reference surface's
     * macroblocks in the holes until the next IDR. That is the price, and it is
     * paid on exactly the path wait-for-keyframe=false just opened up.
     *
     * thread-type=slice stays on the fallback: avdec defaults to FRAME
     * threading, which holds output back by (threads-1) frames. */
    GstElementFactory *hwdec = gst_element_factory_find ("d3d11h264dec");
    if (hwdec && gst_plugin_feature_get_rank (GST_PLUGIN_FEATURE (hwdec)) > GST_RANK_NONE)
      dec = "d3d11h264dec ! video/x-raw";
    else
      dec = "avdec_h264 thread-type=slice output-corrupt=false";
    if (hwdec)
      gst_object_unref (hwdec);
    mux = "h264parse ! matroskamux";
  } else if (g_ascii_strcasecmp (encoding, "VP8") == 0) {
    head = "rtpvp8depay";
    dec = "vp8dec";
    mux = "matroskamux";
  } else {
    post_ui (self, "The phone is sending a codec this build cannot decode", NULL);
    gst_caps_unref (caps);
    return;
  }
  gst_caps_unref (caps);

  /* processing-deadline=0 because the 15 ms every GstVideoSink starts with is
   * not slack, it is delay. gst_video_sink_init hands its subclasses 15 ms
   * (gstvideosink.c:175-176 in the 1.28.7 this bundle ships, over GstBaseSink's
   * own 20 at gstbasesink.c:309), and gst_base_sink_query_latency adds that to
   * whatever upstream reported whenever the sink syncs to the clock: "min +=
   * processing_deadline" at gstbasesink.c:1246, reached only through "l =
   * sink->sync" at :1214. The pipeline adopts the sum and tells every element,
   * and the log says so in two lines: the jitterbuffer answers "Our latency:
   * 0:00:00.200000000" and is then told "configuring latency of
   * 0:00:00.215000000" (gstrtpjitterbuffer.c:2065). Those 15 ms are the whole
   * difference between the two, and the sink adds the 215 to each buffer's
   * running time before it waits on the clock, so all the deadline does here is
   * make every frame due 15 ms later. It is meant to cover the time a sink needs
   * to get a frame onto the glass, and this one needs none of it:
   * gtk4paintablesink's render hands the frame to the GTK main loop and returns,
   * and GTK paints on its own frame clock whether we waited or not. Nothing
   * upstream loses a window for it either: the jitterbuffer's own 200 ms is its
   * "latency" property, and the only use it has for the latency event is
   * stretching its delay in buffer mode (gstrtpjitterbuffer.c:2072), which is
   * not the slave mode rtpbin defaults to and webrtcbin leaves alone. What it
   * costs is the frames that arrive inside the 15 ms being given back: over 28
   * minutes of receiver8.gst.log, 49,427 frames, five had their last packet in
   * later than pts + 200 ms and three of those were already late against
   * pts + 215. The worst was in at pts + 227, still inside the drop test, which
   * only discards a frame past its deadline plus its own duration plus
   * max-lateness (gstbasesink.c:3127), some 38 ms further out. So a couple of
   * frames per half hour paint late instead of on the beat, and none vanish. */
  gchar *desc = g_strdup_printf (
      "%s ! tee name=t allow-not-linked=true "
      "t. ! queue max-size-buffers=3 max-size-time=0 max-size-bytes=0 ! %s ! videoconvert ! "
      "gtk4paintablesink name=vsink processing-deadline=0", head, dec);
  GError *error = NULL;
  GstElement *tail = gst_parse_bin_from_description (desc, TRUE, &error);
  g_free (desc);
  if (error) {
    gchar *msg = g_strdup_printf ("Failed to build the %s tail: %s", encoding, error->message);
    post_ui (self, msg, NULL);
    g_free (msg);
    g_error_free (error);
    return;
  }

  GstElement *sink = gst_bin_get_by_name (GST_BIN (tail), "vsink");
  if (!sink) {
    post_ui (self, "gtk4paintablesink is missing — see receiver/README.md", NULL);
    gst_object_unref (tail);
    return;
  }
  /* The tee stays reachable so the record button can graft a branch onto it
   * mid-session. Both are owned by the bin, hence no extra ref kept here. */
  GstElement *tee = gst_bin_get_by_name (GST_BIN (tail), "t");
  self->tee = tee;
  self->record_mux = g_strdup (mux);

  gst_bin_add (GST_BIN (self->pipeline), tail);
  gst_element_sync_state_with_parent (tail);

  GstPad *tail_sink = gst_element_get_static_pad (tail, "sink");
  if (gst_pad_link (pad, tail_sink) != GST_PAD_LINK_OK)
    post_ui (self, "Failed to link the decoder", NULL);
  gst_object_unref (tail_sink);

  PaintableHandover *handover = g_new0 (PaintableHandover, 1);
  handover->self = self;
  handover->sink = sink;        /* ref transferred; attach_paintable unrefs it */
  if (tee)
    gst_object_unref (tee);
  g_idle_add (attach_paintable, handover);
}

/* ---------------------------------------------------------------------- main */

static GtkWidget *
toolbar_button (const gchar *icon, const gchar *tooltip)
{
  GtkWidget *button = gtk_button_new_from_icon_name (icon);
  gtk_widget_set_tooltip_text (button, tooltip);
  gtk_widget_add_css_class (button, "tool");
  return button;
}

/* ------------------------------------------------------------------ update */

/* Nothing here downloads or runs anything. The verified manifest yields three
 * integers; those build a GitHub tag URL, and the browser does the rest — where
 * Mark of the Web and SmartScreen still apply to whatever the user chooses to
 * run. docs/threat-model.md says what that does and does not buy. */
static void
on_update_checked (GObject *source, GAsyncResult *result, gpointer user_data)
{
  App *self = user_data;
  AircastUpdate *update = aircast_update_check_finish (result);

  g_clear_pointer (&self->update_url, g_free);
  if (update->available && update->tag_url) {
    self->update_url = g_strdup (update->tag_url);
    /* Built with printf from validated integers and hex, never set_markup, and
     * never a sentence the feed wrote: this label is the one place a user
     * decides whether to go and install something. */
    gchar *text = g_strdup_printf ("%s — click Update.\nFile: %s\nSHA-256: %s",
        update->status, update->asset, update->sha256);
    gtk_label_set_text (GTK_LABEL (self->update_label), text);
    g_free (text);
  } else {
    gint64 now = g_get_real_time () / G_USEC_PER_SEC;
    gint64 days = update->last_success ? (now - update->last_success) / 86400 : -1;

    if (update->configured && days > 30) {
      /* A check that never succeeds again is how a freeze looks from inside the
       * app, and silence is what makes it work. */
      gchar *text = g_strdup_printf (
          "%s — no successful check in %" G_GINT64_FORMAT " days; "
          "see github.com/napejoon/aircast_ws/releases", update->status, days);
      gtk_label_set_text (GTK_LABEL (self->update_label), text);
      g_free (text);
    } else {
      gtk_label_set_text (GTK_LABEL (self->update_label), update->status);
    }
  }
  aircast_update_free (update);
}

static void
on_update_clicked (GtkButton *button, App *self)
{
  if (self->update_url) {
    GtkUriLauncher *launcher = gtk_uri_launcher_new (self->update_url);
    gtk_uri_launcher_launch (launcher, GTK_WINDOW (self->window), NULL, NULL, NULL);
    g_object_unref (launcher);
    return;
  }
  gtk_label_set_text (GTK_LABEL (self->update_label), "Checking for updates…");
  aircast_update_check_async (AIRCAST_VERSION, TRUE, on_update_checked, self);
}

/* -------------------------------------------------------- wireless display */

#ifdef G_OS_WIN32
/* The tablet's own Smart View is Miracast, and Miracast is not something this
 * program can become: it wants Wi-Fi Direct, an RTSP handshake, HDCP and a WLAN
 * driver willing to act as a sink, none of which webrtcbin has any part of.
 * Windows already ships that sink and merely leaves it out of the image, so the
 * honest offer is a door to it rather than an imitation of it.
 * docs/research/smart-view-miracast.md has the verdict and what it costs.
 *
 * ShellExecuteW, and not the GtkUriLauncher the Update button uses a few lines
 * up, because this scheme is not launched from a command line at all:
 * HKCR\ms-settings\Shell\Open\Command holds no default value, only
 * DelegateExecute={4ed3a719-cea8-4bd9-910d-e252f997afc2}, and the sibling
 * Shell\Open keys name a packaged app by ActivatableClassId and PackageId.
 * Whether GIO can follow that chain has not been tested here; the shell
 * certainly can, and it is the component that owns the chain.
 *
 * COM before the call, and on this thread, because that CLSID is an in-process
 * server (windows.system.launcher.dll, ThreadingModel Both) which the shell has
 * to CoCreateInstance on whichever thread called it. A thread with no apartment
 * gets CO_E_NOTINITIALIZED and a return of 32 or less, which is
 * indistinguishable from Settings being missing and would make this button fail
 * everywhere while blaming Windows. We release only the reference we actually
 * took: RPC_E_CHANGED_MODE means GTK initialised the apartment first with the
 * other model, which is fine here precisely because the handler is
 * ThreadingModel=Both, and uninitialising then would be tearing down GTK's.
 *
 * Nothing here asks first whether the feature is installed. Settings is the
 * only thing on the machine that knows without elevation, and that page already
 * says to add the Wireless Display optional feature, with the button under it,
 * when the answer is no. A probe of our own could only be a second opinion that
 * disagrees with the page we are about to open, and a wrong one would hide this
 * button on exactly the machines that need it. */
static void
on_wireless_display_clicked (GtkButton *button, App *self)
{
  HRESULT com = CoInitializeEx (NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

  if ((INT_PTR) ShellExecuteW (NULL, L"open", L"ms-settings:project", NULL, NULL,
      SW_SHOWNORMAL) <= 32)
    set_status (self, "Could not open Settings. Open it yourself: "
        "System > Projecting to this PC.");

  if (SUCCEEDED (com))
    CoUninitialize ();
}
#endif

static GtkWidget *
build_idle_page (App *self)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_halign (box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (box, "card");

  GtkWidget *title = gtk_label_new ("aircast");
  gtk_widget_add_css_class (title, "title");

  GtkWidget *hint = gtk_label_new ("On your phone, open aircast and enter this code");
  gtk_widget_add_css_class (hint, "hint");

  self->code_label = gtk_label_new (self->code);
  gtk_widget_add_css_class (self->code_label, "code");
  gtk_label_set_selectable (GTK_LABEL (self->code_label), TRUE);

  self->status_label = gtk_label_new ("Connecting to the signalling server…");
  gtk_widget_add_css_class (self->status_label, "status");
  gtk_label_set_wrap (GTK_LABEL (self->status_label), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (self->status_label), 48);
  gtk_label_set_justify (GTK_LABEL (self->status_label), GTK_JUSTIFY_CENTER);

  gtk_box_append (GTK_BOX (box), title);
  gtk_box_append (GTK_BOX (box), hint);
  gtk_box_append (GTK_BOX (box), self->code_label);
  self->update_label = gtk_label_new ("");
  gtk_widget_add_css_class (self->update_label, "status");
  gtk_label_set_wrap (GTK_LABEL (self->update_label), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (self->update_label), 48);
  gtk_label_set_justify (GTK_LABEL (self->update_label), GTK_JUSTIFY_CENTER);
  gtk_label_set_selectable (GTK_LABEL (self->update_label), TRUE);

  gtk_box_append (GTK_BOX (box), self->status_label);
  gtk_box_append (GTK_BOX (box), self->update_label);

#ifdef G_OS_WIN32
  /* On the idle card and nowhere else: this is the screen someone stares at
   * when the phone in their hand has no aircast on it, and by the time there is
   * a live session the question has answered itself. Flat, and wearing the
   * hint's grey rather than a style of its own, because it is the way out of
   * this window and not the way we want anyone to cast: what it opens has no
   * recording, no bezel, and a latency --latency cannot reach. */
  GtkWidget *wireless =
      gtk_button_new_with_label ("No app on the phone? Use Windows Wireless Display");
  gtk_button_set_has_frame (GTK_BUTTON (wireless), FALSE);
  gtk_widget_add_css_class (wireless, "hint");
  gtk_widget_set_tooltip_text (wireless,
      "Opens Settings > System > Projecting to this PC, where Windows' own "
      "Miracast receiver is installed and switched on. Adding it needs an "
      "administrator once. The picture is then Windows': aircast cannot record "
      "it or tune its latency.");
  g_signal_connect (wireless, "clicked", G_CALLBACK (on_wireless_display_clicked), self);
  gtk_box_append (GTK_BOX (box), wireless);
#endif

  return box;
}

static GtkWidget *
build_live_page (App *self)
{
  /* The bezel is the Reflector cue that this is a phone and not a window: a
   * thick dark rounded frame the video is clipped into. */
  GtkWidget *bezel = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (bezel, "bezel");
  gtk_widget_set_halign (bezel, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (bezel, GTK_ALIGN_CENTER);

  self->picture = gtk_picture_new ();
  gtk_picture_set_content_fit (GTK_PICTURE (self->picture), GTK_CONTENT_FIT_CONTAIN);
  gtk_widget_set_hexpand (self->picture, TRUE);
  gtk_widget_set_vexpand (self->picture, TRUE);
  gtk_widget_add_css_class (self->picture, "screen");

  gtk_box_append (GTK_BOX (bezel), self->picture);
  return bezel;
}

static GtkWidget *
build_toolbar (App *self)
{
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class (bar, "toolbar");

  self->record_button = gtk_toggle_button_new ();
  gtk_button_set_icon_name (GTK_BUTTON (self->record_button), "media-record-symbolic");
  gtk_widget_set_tooltip_text (self->record_button, "Record (R)");
  gtk_widget_add_css_class (self->record_button, "tool");
  g_signal_connect (self->record_button, "toggled", G_CALLBACK (on_record_toggled), self);

  self->record_time = gtk_label_new ("00:00");
  gtk_widget_add_css_class (self->record_time, "rec-time");
  gtk_widget_set_visible (self->record_time, FALSE);

  GtkWidget *fullscreen = toolbar_button ("view-fullscreen-symbolic", "Fullscreen (F)");
  g_signal_connect (fullscreen, "clicked", G_CALLBACK (on_fullscreen_clicked), self);

  GtkWidget *quit = toolbar_button ("window-close-symbolic", "Disconnect");
  g_signal_connect (quit, "clicked", G_CALLBACK (on_disconnect_clicked), self);

  GtkWidget *update = gtk_button_new_with_label ("Update");
  gtk_widget_set_tooltip_text (update, "Check for a new version");
  gtk_widget_add_css_class (update, "tool");
  g_signal_connect (update, "clicked", G_CALLBACK (on_update_clicked), self);

  gtk_box_append (GTK_BOX (bar), self->record_button);
  gtk_box_append (GTK_BOX (bar), self->record_time);
  gtk_box_append (GTK_BOX (bar), update);
  gtk_box_append (GTK_BOX (bar), fullscreen);
  gtk_box_append (GTK_BOX (bar), quit);
  return bar;
}

static void
load_css (void)
{
  GtkCssProvider *provider = gtk_css_provider_new ();
  /* style.css is compiled in as a GResource-free string: one file, one place
   * to edit the look, no install step. */
  gtk_css_provider_load_from_string (provider,
#include "style.css.h"
  );
  gtk_style_context_add_provider_for_display (gdk_display_get_default (),
      GTK_STYLE_PROVIDER (provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (provider);
}

static void
activate (GtkApplication *app, gpointer user_data)
{
  App *self = user_data;

  load_css ();

  self->window = gtk_application_window_new (app);
  gtk_window_set_title (GTK_WINDOW (self->window), "aircast");
  gtk_window_set_default_size (GTK_WINDOW (self->window), 1100, 760);
  gtk_widget_add_css_class (self->window, "room");

  self->stack = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (self->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_add_named (GTK_STACK (self->stack), build_idle_page (self), "idle");
  gtk_stack_add_named (GTK_STACK (self->stack), build_live_page (self), "live");

  self->revealer = gtk_revealer_new ();
  gtk_revealer_set_transition_type (GTK_REVEALER (self->revealer),
      GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
  gtk_revealer_set_child (GTK_REVEALER (self->revealer), build_toolbar (self));
  gtk_widget_set_halign (self->revealer, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (self->revealer, GTK_ALIGN_END);
  gtk_widget_set_margin_bottom (self->revealer, 28);

  GtkWidget *overlay = gtk_overlay_new ();
  gtk_overlay_set_child (GTK_OVERLAY (overlay), self->stack);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), self->revealer);
  gtk_window_set_child (GTK_WINDOW (self->window), overlay);

  GtkEventController *motion = gtk_event_controller_motion_new ();
  g_signal_connect (motion, "motion", G_CALLBACK (on_motion), self);
  gtk_widget_add_controller (self->window, motion);

  GtkEventController *keys = gtk_event_controller_key_new ();
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key_pressed), self);
  gtk_widget_add_controller (self->window, keys);

  gtk_window_present (GTK_WINDOW (self->window));

  /* Throttled to one attempt per day and keyed on attempts, so neither a
   * hostile network nor a restart loop can spin it. */
  aircast_update_check_async (AIRCAST_VERSION, FALSE, on_update_checked, self);

  self->session = soup_session_new ();
  self->message = soup_message_new (SOUP_METHOD_GET, self->signal_url);
  soup_session_websocket_connect_async (self->session, self->message, NULL, NULL,
      G_PRIORITY_DEFAULT, NULL, on_connected, self);
}

static void
shutdown_app (GtkApplication *app, gpointer user_data)
{
  App *self = user_data;

  if (self->recording)
    stop_recording (self);
  if (self->ws)
    soup_websocket_connection_close (self->ws, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
  if (self->pipeline) {
    /* The watch comes off before the EOS goes in. A bus watch drains every
     * message, so with it still installed the EOS below is consumed by the
     * handler and never reaches this pop, and a wait that normally returns in
     * milliseconds becomes a flat three-second pause on every quit. */
    GstBus *bus = gst_element_get_bus (self->pipeline);
    gst_bus_remove_watch (bus);
    /* An EOS is what closes a recording's container cleanly. */
    gst_element_send_event (self->pipeline, gst_event_new_eos ());
    gst_bus_timed_pop_filtered (bus, 3 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    gst_object_unref (bus);
    gst_element_set_state (self->pipeline, GST_STATE_NULL);
    gst_object_unref (self->pipeline);
    self->pipeline = NULL;
  }
}

int
main (int argc, char *argv[])
{
  /* The jitter buffer's latency is the single largest term in glass-to-glass
   * delay on a relayed pair, and it is not a buffer: rtpjitterbuffer pushes an
   * in-order packet the moment it arrives. The number is a deadline in two
   * places at once. It is when a missing packet is finally called lost, and it
   * is what the sink adds to every frame's presentation time before it waits on
   * the clock, so it is real delay and it comes off one-for-one.
   *
   * 200 is the knee of a measured curve, not slack above it. Over one run the
   * gap between a retransmission being asked for and arriving was 142 ms at the
   * median, 190 at p90 and 217 at p99 — the relay round trip is 43 ms to the
   * notebook and 52 ms to the tablet before anything else. At 200 ms, 9 of 234
   * loss bursts missed the deadline, one smeared frame every 82 seconds; at 160
   * it is one every 20 seconds, and at 150 one every 10. That buys 40 or 50 ms
   * off a budget of three or four hundred, which nobody sees, and pays for it
   * in the exact sharpness that was just fixed.
   *
   * --latency still walks it either way, and it is worth re-walking: the run
   * that made 120 ms stutter predates every fix since. Below about 120 the
   * frames themselves start arriving past their own render time. */
  App self = { .latency_ms = 200 };

  /* First statement in main(): before gst_init() runs inside the option parse,
   * and before anything can cache a data directory. */
  harden_environment ();

  GOptionEntry entries[] = {
    { "signal", 's', 0, G_OPTION_ARG_STRING, &self.signal_url,
        "Signalling server URL (default: " AIRCAST_SIGNAL ")", "URL" },
    { "code", 'c', 0, G_OPTION_ARG_STRING, &self.code,
        "6-digit pairing code (generated and shown if omitted)", "CODE" },
    { "record-dir", 'r', 0, G_OPTION_ARG_FILENAME, &self.record_dir,
        "Where the record button writes .mkv files (default: home)", "DIR" },
    { "latency", 'l', 0, G_OPTION_ARG_INT, &self.latency_ms,
        "Jitter buffer in ms (default 200, the first knob to tune)", "MS" },
    { "insecure", 0, 0, G_OPTION_ARG_NONE, &self.insecure,
        "Allow a plaintext ws:// signalling URL. LAN bring-up only", NULL },
    { "selftest", 0, 0, G_OPTION_ARG_NONE, &self.selftest,
        "Run the version and signature self-tests and exit", NULL },
    { "verify-manifest", 0, 0, G_OPTION_ARG_FILENAME, &self.verify_manifest,
        "Verify an update manifest against this build's key and exit", "FILE" },
    { "verify-signature", 0, 0, G_OPTION_ARG_FILENAME, &self.verify_signature,
        "The .minisig for --verify-manifest", "FILE" },
    { "relay-only", 0, 0, G_OPTION_ARG_NONE, &self.relay_only,
        "Send everything through the TURN relay, never directly. Slower, and "
        "the sender needs AIRCAST_RELAY=true to match", NULL },
    { NULL },
  };

  GOptionContext *ctx = g_option_context_new ("- aircast receiver");
  g_option_context_add_main_entries (ctx, entries, NULL);
  g_option_context_add_group (ctx, gst_init_get_option_group ());
  GError *error = NULL;
  if (!g_option_context_parse (ctx, &argc, &argv, &error)) {
    g_printerr ("%s\n", error->message);
    return 1;
  }
  g_option_context_free (ctx);

  /* Both exits happen before any window, so CI runs them headless. */
  if (self.selftest)
    return aircast_update_selftest ();
  if (self.verify_manifest || self.verify_signature) {
    if (!self.verify_manifest || !self.verify_signature) {
      g_printerr ("--verify-manifest and --verify-signature go together\n");
      return 1;
    }
    return aircast_update_selftest_manifest (self.verify_manifest, self.verify_signature);
  }

  /* The default is compiled in so that the installed program is a program:
   * double-click the shortcut and it works, with no terminal and no flags.
   * --signal stays, because anyone running their own relay needs it, and a
   * build with the define empty still demands one rather than guessing. */
  if (!self.signal_url) {
    if (AIRCAST_SIGNAL[0] == '\0') {
      g_printerr ("--signal is required: this build has no default\n");
      return 1;
    }
    self.signal_url = g_strdup (AIRCAST_SIGNAL);
  }

  /* wss:// only. A plaintext signalling channel hands the pairing code and both
   * SDPs to anyone on the path, and what follows them is a screen. */
  if (!g_str_has_prefix (self.signal_url, "wss://") && !self.insecure) {
    g_printerr ("--signal must be wss://; pass --insecure to allow ws:// on a "
        "LAN you control\n");
    return 1;
  }
  if (!self.code) {
    /* Nobody has to invent a code: the receiver picks one and shows it, and
     * the phone types it in.
     *
     * g_random_int_range is not a CSPRNG, and 10^6 codes is a guessable space
     * either way, so the defence has to be server-side: the code lives 300 s
     * and the server rate-limits joins. Do not treat this value as a secret. */
    self.code = g_strdup_printf ("%06u", g_random_int_range (0, 1000000));
  }

  if (self.relay_only)
    g_printerr ("--relay-only: every packet goes through the TURN relay, which "
        "costs a round trip the two peers may not need. The sender needs "
        "--dart-define=AIRCAST_RELAY=true or it will offer host candidates "
        "this side will not pair with.\n");

  gtk_init ();

  self.app = gtk_application_new ("io.aircast.receiver", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect (self.app, "activate", G_CALLBACK (activate), &self);
  g_signal_connect (self.app, "shutdown", G_CALLBACK (shutdown_app), &self);
  /* GtkApplication would otherwise try to parse our own arguments again. */
  int status = g_application_run (G_APPLICATION (self.app), 0, NULL);

  g_object_unref (self.app);
  return status;
}
