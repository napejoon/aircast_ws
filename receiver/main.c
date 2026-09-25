/* Kagami — GTK4 window around a GStreamer webrtcbin.
 *
 * The binary, the GApplication id, the pairing scheme and the update
 * manifest's product key were renamed with the program, which orphans every
 * copy already installed: a phone running the old sender scans a kagami://
 * code it does not recognise, and an old manifest names a product this build
 * is not. That was the deliberate price of one name instead of two. The
 * signalling host, the update manifest's own filename and the aircast_ C
 * prefix are not the program's name -- they are a deployed domain, a release
 * asset and an internal API -- and none of them moved.
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
#ifdef G_OS_WIN32
#include <gdk/win32/gdkwin32.h>
#endif
#include <qrencode.h>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>

#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <stdio.h>              /* freopen and setvbuf, for the log file */
#include <string.h>

#ifdef G_OS_WIN32
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#define COBJMACROS
#include <d3d11.h>
#endif

#include "update_check.h"


/* The two jitter buffer sizes, in milliseconds, that the measurements on this
 * deployment left standing. choose_latency() picks between them from the pair
 * ICE selected; the case for each is written where it is used, the relay
 * number in main() and the direct one above choose_latency(). */
#define AIRCAST_LATENCY_DIRECT 60
#define AIRCAST_LATENCY_RELAY 200

typedef struct {
  /* configuration */
  gchar *signal_url;
  gchar *code;
  gchar *record_dir;
  /* What the user typed, or negative if they typed nothing. choose_latency()
   * never writes here: its answer lives in latency_chosen, so a second cast
   * over a different path is free to reach a different one. */
  gint latency_ms;
  gint latency_chosen;
  gboolean relay_only;
  gboolean insecure;
  gboolean selftest;
  gboolean prebuild_registry;
  gchar *verify_manifest;
  gchar *verify_signature;

  /* signalling */
  SoupSession *session;
  SoupMessage *message;
  SoupWebsocketConnection *ws;
  /* Seconds to wait before the next signalling attempt, doubling to a ceiling,
   * and the pending timer so a second failure does not stack another one. */
  guint reconnect_delay;
  guint reconnect_source;

  /* media */
  GstElement *pipeline;
  GstElement *webrtc;
  GstElement *tee;              /* owned by the tail bin */
  GstElement *record_branch;
  GstPad *record_tee_pad;
  gchar *record_mux;            /* the muxer half of the tail, per codec */
  gchar *record_file;
  gboolean drop_pending;        /* drop_session is waiting on drop_record_branch */
  /* The TURN block the server sent with "joined". Kept because that message
   * arrives once per socket and build_pipeline is the only thing that reads
   * it, so a session that ends while the socket lives needs it a second time. */
  JsonObject *turn;

  /* interface */
  GtkApplication *app;
  GtkWidget *window;
  GtkWidget *stack;
  GtkWidget *picture;
  GtkWidget *code_label;
  GtkWidget *status_label;
  GtkWidget *record_button;
  GtkWidget *record_time;
  /* The same text as status_label, in the toolbar. status_label lives on the
   * idle card, and the stack is showing the live page whenever the recording
   * messages at main.c 296, 332, 363 and 425 fire. */
  GtkWidget *live_status;

  /* The strip along the bottom of the window. It is always there, on both
   * pages, because the question it answers -- what is this connection doing
   * right now -- is the one this program exists to get right, and until it
   * existed the answer was a twelve-pixel grey line that said "Negotiating"
   * and then went blank. */
  GtkWidget *strip_dot;         /* beacon: grey idle, green live, red failed */
  GtkWidget *strip_state;       /* "Waiting for a phone" / "Mirroring" */
  GtkWidget *strip_path;        /* "Direct - host" / "Relay" */
  GtkWidget *strip_latency;     /* round trip, from get-stats */
  GtkWidget *strip_buffer;      /* the jitter buffer this cast settled on */
  GtkWidget *strip_res;         /* what the encoder is sending, right now */
  GtkWidget *strip_loss;        /* packets lost, as a share of those sent */
  GtkWidget *toolbar;           /* below the video rather than floating over it */
  GtkWidget *strip;             /* the whole bottom bar, hidden in fullscreen */
  GtkWidget *strip_readings;    /* the six cells, foldable as a group */
  gboolean readings_wanted;     /* what D last asked for, across page switches */
  GtkWidget *fullscreen_button; /* its icon turns over with the state */
  gboolean strip_was_shown;     /* folded state, remembered across fullscreen */
  gboolean was_maximized;       /* window state to restore when fullscreen ends */
  gint keyframe_wanted;         /* atomic: set on the streaming thread, read on the main one */
  GtkWidget *strip_toggle;      /* the chevron that folds them */
  guint stats_timer;            /* 1 Hz while a session is up, 0 otherwise */
  guint64 last_lost, last_recv; /* so loss reads per second, not per session */
  GtkWidget *update_label;      /* passive: last checked, highest version seen */
  gchar *update_url;            /* built from verified integers, or NULL */

  guint record_timer;
  /* The 700 ms the muxer gets to close its file. Kept so a session that ends
   * inside that window can cancel it rather than let it fire into a pipeline
   * that has been freed. */
  guint record_drop_timer;
  gint64 record_started;
  gboolean recording;
  /* Disconnect sent a "bye" and the phone answers it with one of its own
   * (signaling.dart close()); that echo must not rewrite the status. */
  gboolean disconnecting;
} App;

/* Monotonic, from the first statement in main(), so the numbers in the log
 * are wall time from launch and can be read against a stopwatch.
 *
 * Startup has been reported slow -- ten seconds from the Start menu -- and
 * measuring it from outside the process cannot say which part. A launch from
 * a shortcut reached its window in 15.2 s on the machine it was reported
 * from and in 1.4 s on the next launch, which is the shape of something
 * touching the filesystem cold, and there are four candidates between main()
 * and the first frame. These five lines cost nothing and end the guessing:
 * whoever sees it slow again can send the log. */
static gint64 startup_us;

static void
mark (const gchar *what)
{
  /* g_printerr, not g_message: the log file this ends up in is a freopen of
   * stderr and nothing redirects stdout, which is where GLib's default
   * handler puts a message. */
  g_printerr ("startup: %s at %.2f s\n", what,
      (g_get_monotonic_time () - startup_us) / 1e6);
}

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
harden_environment (gboolean prebuild)
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

  /* Windows draws this window's frame, not GTK.
   *
   * GTK's own title bar looks near enough to the real one to be mistaken for
   * it, and then behaves nothing like it: dragging the window into the side
   * of the screen does not snap it to half, the top does not maximise it, and
   * Win+arrow reaches it only through the window manager's fallbacks. All of
   * that is Aero Snap, all of it belongs to the frame, and a client-drawn
   * frame does not have it. This window has no header bar and puts nothing in
   * the title but its name, so it was paying for a frame it does not use.
   *
   * Set rather than overridden, so GTK_CSD from the environment still wins,
   * and set here because gtk_window_constructed reads it once, at the moment
   * the first window is made. */
  g_setenv ("GTK_CSD", "0", FALSE);

  gchar *root = g_win32_get_package_installation_directory_of_module (NULL);
  if (root) {
    gchar *schemas = g_build_filename (root, "share", "glib-2.0", "schemas", NULL);
    g_setenv ("GSETTINGS_SCHEMA_DIR", schemas, TRUE);
    g_free (schemas);

    /* The registry the installer built, if it is there. Measured on the shipped
     * bundle: with no cache gst_init spends 0.99 s rebuilding it on a warm
     * filesystem and 4.82 s on a cold one, because a rebuild spawns
     * gst-plugin-scanner and LoadLibrary's every plugin through a Defender that
     * has not seen them. With a cache it is 0.02 s and the plugins load lazily,
     * when a pipeline first asks for an element, which is after the window.
     *
     * The installer runs --prebuild-registry as SYSTEM once the files are
     * down, so the first launch of the first user already finds it. The
     * existence test is what keeps the zip bundle and a build tree on their
     * per-user cache: pointing GST_REGISTRY at a file nobody can write is how
     * you get a rescan on every launch instead of one.
     *
     * ponytail: a registry that goes stale under a read-only install (someone
     * replaces a plugin DLL by hand) rescans every launch and says so only in
     * the log. Re-running the installer rebuilds it; detecting it here would
     * cost the stat of all 18 plugins that the cache exists to avoid. */
    gchar *registry = g_build_filename (root, "registry.bin", NULL);
    if (prebuild || g_file_test (registry, G_FILE_TEST_EXISTS))
      g_setenv ("GST_REGISTRY", registry, TRUE);
    g_free (registry);
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
/* One RTCP PLI; defined beside the repair loop that is its other caller. */
static void request_keyframe (App *self);
/* Ends a media session without ending the signalling one: defined below, next
 * to the teardown it shares with a lost socket. */
static void drop_session (App *self);
/* Defined beside the strip they drive, used from the session code above it. */
static void set_strip_state (App *self, const gchar *css, const gchar *text);
static gboolean poll_stats (gpointer data);
static void on_strip_toggled (GtkButton *button, App *self);
static void apply_strip_readings (App *self);

/* ----------------------------------------------------------------- interface */

static void
show_page (App *self, const gchar *page)
{
  gtk_stack_set_visible_child_name (GTK_STACK (self->stack), page);
  /* Without this the pill carries the last idle-page line over the live video
   * for the whole cast. "Negotiating" (main.c:939) is the one that lands there
   * every successful session, because show_page("live") follows it. Nothing is
   * lost by clearing here: apply_ui_update sets the text before it changes the
   * page, and both post_ui calls that pass a page pass "idle". */
  if (self->live_status)
    gtk_label_set_text (GTK_LABEL (self->live_status), "");
  /* The toolbar belongs to the live page and nothing in it means anything on
   * the idle one -- Disconnect from a session that does not exist, Record with
   * no picture to record. */
  if (self->toolbar)
    gtk_widget_set_visible (self->toolbar, g_str_equal (page, "live"));
  apply_strip_readings (self);
  /* And fullscreen belongs to the mirror, so leaving the mirror leaves it.
   *
   * The line above is what makes this necessary: the only visible way out of
   * fullscreen is the button on that toolbar, and hiding the toolbar while the
   * window is still fullscreen leaves an idle card on a screen with no title
   * bar, no close button and no control of any kind. on_fullscreen_changed
   * says a fullscreen window with no visible way out is a trap and keeps the
   * toolbar for exactly that reason; this path was taking it away again.
   *
   * It is reached by every route off the live page -- Disconnect, the phone
   * hanging up, a failed connection, and the one that found it: the signalling
   * socket closing mid-cast, which shows the pairing code again under
   * "Reconnecting" with nothing to press. */
  if (self->window && !g_str_equal (page, "live")
      && gtk_window_is_fullscreen (GTK_WINDOW (self->window)))
    gtk_window_unfullscreen (GTK_WINDOW (self->window));
}

static void
set_status (App *self, const gchar *text)
{
  gtk_label_set_text (GTK_LABEL (self->status_label), text);
  /* build_toolbar runs after build_idle_page, so this is NULL for the first
   * set_status calls of the run -- which are idle-page messages anyway. */
  if (self->live_status)
    gtk_label_set_text (GTK_LABEL (self->live_status), text);
}

/* GStreamer callbacks arrive on streaming threads; GTK may only be touched
 * from the main one. Everything the pipeline wants to say to the UI goes
 * through one of these. */
typedef struct {
  App *self;
  gchar *text;
  gchar *page;
  /* The beacon and the word beside it. strip_text is what decides whether
   * this update touches them at all, because strip_css NULL is meaningful:
   * it is the grey. */
  gchar *strip_css;
  gchar *strip_text;
} UiUpdate;

static gboolean
apply_ui_update (gpointer data)
{
  UiUpdate *update = data;
  if (update->text)
    set_status (update->self, update->text);
  if (update->page)
    show_page (update->self, update->page);
  if (update->strip_text)
    set_strip_state (update->self, update->strip_css, update->strip_text);
  g_free (update->text);
  g_free (update->page);
  g_free (update->strip_css);
  g_free (update->strip_text);
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

/* The same hop, for the one thing on this window that is only ever changed
 * from a streaming thread and is not a sentence. */
static void
post_strip (App *self, const gchar *css, const gchar *text)
{
  UiUpdate *update = g_new0 (UiUpdate, 1);
  update->self = self;
  update->strip_css = g_strdup (css);
  update->strip_text = g_strdup (text);
  g_idle_add (apply_ui_update, update);
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

  /* g_build_filename copies its arguments, so the printed name has an owner
   * here and nowhere else, and the GDateTime the stamp came out of has one
   * too. Both were leaked on every recording. */
  GDateTime *now = g_date_time_new_now_local ();
  gchar *stamp = g_date_time_format_iso8601 (now);
  g_date_time_unref (now);
  g_strdelimit (stamp, ":", '-');
  gchar *name = g_strdup_printf ("kagami-%s.mkv", stamp);
  g_free (stamp);
  g_free (self->record_file);
  /* The Videos folder, not the home directory. It is where a recording is
   * looked for, and it is resolved from the known-folder API rather than from
   * the environment: g_get_home_dir honours HOME, and a Git Bash launch hands
   * over HOME=/c/Users/<name>, which Windows reads as C:\c\Users\<name> --
   * a folder that does not exist, so filesink fails to open and the record
   * button presses into nothing. */
  const gchar *dir = self->record_dir;
  if (!dir)
    dir = g_get_user_special_dir (G_USER_DIRECTORY_VIDEOS);
  if (!dir)
    dir = g_get_home_dir ();
  self->record_file = g_build_filename (dir, name, NULL);
  g_free (name);

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

  /* Ask the phone for a keyframe now. The branch is grafted on wherever the
   * stream happens to be, and libwebrtc emits an IDR only at stream start or on
   * a PLI -- the same fact rtph264depay's request-keyframe exists for. Without
   * one the branch's h264parse has no SPS/PPS to build the codec_data
   * matroskamux needs, so the file stays empty until some unrelated packet loss
   * orders a keyframe. On a clean direct pair that can be the whole recording,
   * and the teardown still reports it saved. */
  request_keyframe (self);

  self->recording = TRUE;
  self->record_started = g_get_monotonic_time ();
  self->record_timer = g_timeout_add_seconds (1, tick_record_time, self);
  gtk_widget_add_css_class (self->record_button, "recording");
  gtk_widget_set_visible (self->record_time, TRUE);

  gchar *msg = g_strdup_printf ("Recording to %s", self->record_file);
  set_status (self, msg);
  g_free (msg);
}

static gboolean
drop_record_branch (gpointer data)
{
  App *self = data;

  self->record_drop_timer = 0;

  /* drop_session clears all of these and cancels this timer when a session ends
   * inside the 700 ms, so reaching here with no branch means the pipeline went
   * first and the muxer never got to write its index. Saying "Saved" then would
   * be telling the user a truncated file is on disk. */
  if (!self->record_branch && !self->record_tee_pad) {
    set_status (self, "The recording was interrupted and may be incomplete");
    return G_SOURCE_REMOVE;
  }

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
  if (self->drop_pending) {
    self->drop_pending = FALSE;
    drop_session (self);
  }
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

  /* The branch is pulled out when its EOS comes back through on_bus_message,
   * which is when the file is actually closed. This is the deadline for that,
   * not the plan: a muxer that never answers would otherwise leave the branch
   * in the pipeline and the button stuck. Five seconds because the old fixed
   * 700 ms was a guess at how long a long recording needs to write its index,
   * and being wrong in this direction now costs a wait rather than a truncated
   * file. */
  self->record_drop_timer = g_timeout_add (5000, drop_record_branch, self);
  return GST_PAD_PROBE_REMOVE;
}

/* One PLI, asked for in the only way this build can ask.
 *
 * Built by hand rather than with gst_video_event_new_upstream_force_key_unit so
 * this does not drag gstreamer-video-1.0 into the link line: the structure name
 * is the whole contract, and rtpsession supplies the fields it does not find.
 * webrtcbin turns the event into an RTCP PLI, and libwebrtc answers a PLI with
 * an IDR -- the only two moments it emits one, the other being stream start. */
static void
request_keyframe (App *self)
{
  if (!self->pipeline)
    return;
  gst_element_send_event (self->pipeline,
      gst_event_new_custom (GST_EVENT_CUSTOM_UPSTREAM,
          gst_structure_new ("GstForceKeyUnit",
              "all-headers", G_TYPE_BOOLEAN, TRUE, NULL)));
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
    /* stop_recording clears self->recording at once, but it hands the branch to
     * a pad probe and a 700 ms timer, and record_branch stays set until that
     * timer has pulled it out of the pipeline. Starting again inside that window
     * overwrites the pointer, and the timer then removes the *new* branch
     * instead: a red dot and a running clock over a file that stopped 700 ms in,
     * and the old branch left in the pipeline for good. Two presses in under a
     * second is all it takes, so the second one is refused rather than
     * half-honoured. */
    if (self->record_branch) {
      gtk_toggle_button_set_active (button, FALSE);
      set_status (self, "Still closing the last recording. Try again in a moment");
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
  if (gtk_window_is_fullscreen (window)) {
    gtk_window_unfullscreen (window);
  } else {
    /* Recorded before the transition, because it is the only moment the answer
     * is certainly right: unfullscreening restores the window's size but not
     * its maximised state, so a mirror opened maximised, put fullscreen and
     * brought back came out as a small window in the middle of the screen. */
    self->was_maximized = gtk_window_is_maximized (window);
    gtk_window_fullscreen (window);
  }
}

/* Deferred out of notify::fullscreened; see the call site for why.
 *
 * ShowWindow rather than gtk_window_maximize, and this is the fix for a window
 * that came back from fullscreen with its bottom under the taskbar. With the
 * native title bar (GTK_CSD=0) GDK's maximize sizes the client area to the
 * work area and then puts the title bar on top of it: 2560x1392 of client on a
 * screen whose work area is 1392 tall, so the bottom 23 px -- the connection
 * strip -- sat under the taskbar. A plain maximize, one deferred by half a
 * second, and unmaximize-then-maximize all measured the same 1392. Asking
 * Windows to maximize the frame itself measured 2560x1369, and the strip is
 * whole. Leaving the restore to GTK brings back 1100x760, not maximized. */
static gboolean
restore_maximized (gpointer window)
{
#ifdef G_OS_WIN32
  GdkSurface *surface = gtk_native_get_surface (GTK_NATIVE (window));
  if (surface) {
    ShowWindow ((HWND) gdk_win32_surface_get_handle (surface), SW_MAXIMIZE);
    return G_SOURCE_REMOVE;
  }
#endif
  gtk_window_maximize (GTK_WINDOW (window));
  return G_SOURCE_REMOVE;
}

/* Fullscreen means the mirror and nothing else.
 *
 * gtk_window_fullscreen takes the title bar and the taskbar, and that used to
 * be the whole of it: the toolbar, the readings and the bezel's own 32 px
 * margin, 14 px padding and drop shadow went on eating the screen, so a
 * "fullscreen" mirror was inset by about a hundred pixels on every side. The
 * chrome is worth its space in a window and worth none of it here.
 *
 * Driven from notify::fullscreened rather than from the button, so it is right
 * however the state changed -- the button, F, F11, Escape, or the window
 * manager doing it on its own.
 *
 * The toolbar stays. Everything else can go because it is only information,
 * but a fullscreen window with no visible way out is a trap, and Escape is not
 * a thing every user tries. The strip's own folded state is remembered across
 * the trip so D is not undone by a visit to fullscreen. */
static void
on_fullscreen_changed (GObject *window, GParamSpec *pspec, App *self)
{
  gboolean full = gtk_window_is_fullscreen (GTK_WINDOW (window));

  if (full) {
    self->strip_was_shown = gtk_widget_get_visible (self->strip);
    /* Also read here, for the route that does not come through the button:
     * a window manager can fullscreen a window on its own. */
    if (gtk_window_is_maximized (GTK_WINDOW (window)))
      self->was_maximized = TRUE;
    gtk_widget_add_css_class (self->window, "immersive");
  } else {
    gtk_widget_remove_css_class (self->window, "immersive");
    /* Unconditional, because the round of this bug before last was chased
     * with a message that printed only inside the branch below -- the branch
     * never ran, and the log said nothing at all. */
    g_message ("left fullscreen: window %dx%d, maximized=%d, was_maximized=%d",
        gtk_widget_get_width (GTK_WIDGET (window)),
        gtk_widget_get_height (GTK_WIDGET (window)),
        gtk_window_is_maximized (GTK_WINDOW (window)), self->was_maximized);
    if (self->was_maximized) {
      self->was_maximized = FALSE;
      /* Not from here. This handler runs inside gtk_window_unfullscreen, and
       * asking for another window state while GDK is still settling the last
       * one made the win32 backend free the toplevel's layout twice: the
       * process died with STATUS_HEAP_CORRUPTION, and the dump put the second
       * free under gtk_window_unfullscreen itself. Before that it painted the
       * fullscreen picture into a window that had already shrunk and answered
       * nothing. The idle runs once the state change is complete. */
      g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, restore_maximized,
          g_object_ref (window), g_object_unref);
    }
  }
  gtk_widget_set_visible (self->strip, full ? FALSE : self->strip_was_shown);
  gtk_widget_set_tooltip_text (self->fullscreen_button,
      full ? "Leave fullscreen (F or Escape)" : "Fullscreen (F)");
  gtk_button_set_icon_name (GTK_BUTTON (self->fullscreen_button),
      full ? "view-restore-symbolic" : "view-fullscreen-symbolic");
}

static void
on_disconnect_clicked (GtkButton *button, App *self)
{
  /* Ends the cast, not the program. The button is labelled Disconnect and it
   * used to close the window, which quit -- so the only way back to the code
   * was to start aircast again. Everything this does the "bye" branch of
   * on_message already did when the phone hung up first; this is the same
   * teardown driven from the other end, and the signalling socket is
   * deliberately left open so the next cast needs no reconnect.
   *
   * The phone is told first. It reads "bye" as the receiver hanging up
   * (signaling.dart:135) and stops its own capture, which is what makes the
   * MediaProjection notification go away rather than leaving a phone quietly
   * mirroring into a window that stopped listening. Sent before the local
   * teardown because send_json posts to the main loop, and drop_session must
   * not have cleared the connection out from under the frame by then. */
  JsonBuilder *b = json_builder_new ();
  json_builder_begin_object (b);
  json_builder_set_member_name (b, "type");
  json_builder_add_string_value (b, "bye");
  json_builder_end_object (b);
  send_json (self, b);        /* takes the builder */
  self->disconnecting = TRUE;

  stop_recording (self);
  show_page (self, "idle");
  set_status (self, "Ready for the next cast");
  drop_session (self);
}

static gboolean
on_key_pressed (GtkEventControllerKey *controller, guint keyval, guint code,
    GdkModifierType state, App *self)
{
  switch (keyval) {
    case GDK_KEY_f:
    case GDK_KEY_F:
    case GDK_KEY_F11:
      on_fullscreen_clicked (NULL, self);
      return TRUE;
    case GDK_KEY_r:
    case GDK_KEY_R:
      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->record_button), !self->recording);
      return TRUE;
    case GDK_KEY_d:
    case GDK_KEY_D:
      on_strip_toggled (NULL, self);
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

/* From an idle source, because on_connection_state runs on a webrtcbin task
 * thread and everything drop_session touches belongs to the main one. */
static gboolean
drop_session_idle (gpointer data)
{
  drop_session (data);
  /* After, not before: drop_session ends every session the same way and sets
   * the neutral state, which is the right one for a cast that simply finished.
   * This is the one caller where it did not finish -- ICE failed -- and the
   * beacon says so until the next cast turns it green. */
  set_strip_state (data, "bad", "Connection failed");
  return G_SOURCE_REMOVE;
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
  /* The phone hung up. Not a failure and not a loss: this is what a peer
   * closing its connection looks like from here, and it was the one state
   * this handler did not read.
   *
   * The protocol has a "bye" for it and the sender does send one, but a
   * message only arrives if the socket is still there to carry it, and
   * every way a phone can leave that does not involve pressing Stop --
   * killed from Recents, out of battery, carried out of range -- ends the
   * media connection without ending anything politely. Until this branch
   * existed all of those left the last frame frozen on the glass under a
   * strip still reading Mirroring, with PATH, BUFFER and PICTURE all still
   * filled in, which is the one thing this window must never say when it
   * is not true.
   *
   * Reported from a real cast: Stop on the phone, and the desktop went on
   * showing the launcher it had last received.
   *
   * Not when we are the ones hanging up: Disconnect tears the pipeline down
   * and this fires on the way, and "The phone stopped casting" would land on
   * top of the "Ready for the next cast" that press had just written. */
  else if (state == GST_WEBRTC_PEER_CONNECTION_STATE_CLOSED
      && !self->disconnecting) {
    post_ui (self, "The phone stopped casting", "idle");
    g_idle_add (drop_session_idle, self);
  }
  /* Not the end of anything yet: ICE calls a connection disconnected while
   * it still has consent checks left to try, and a walk between two access
   * points passes through here and comes back. It gets the amber the strip
   * keeps for the state between working and failed, and whichever of the
   * two branches above it settles into does the deciding. */
  else if (state == GST_WEBRTC_PEER_CONNECTION_STATE_DISCONNECTED)
    post_strip (self, "warn", "The link dropped; waiting");
  else if (state == GST_WEBRTC_PEER_CONNECTION_STATE_FAILED) {
    post_ui (self, "The connection failed — is the TURN relay reachable?", "idle");
    /* Saying so was never enough. webrtcbin goes on sending on a transport
     * whose consent the peer has revoked: receiver16.gst.log holds 180 of those
     * over the fourteen minutes after it reported this very state, receiver15
     * holds 489 over forty. And that finished webrtcbin is the one the next
     * offer would be negotiated on, because build_pipeline runs only when a
     * join is answered, and the server answers a join once per socket.
     *
     * The signalling socket is left alone, and now for a different reason than
     * the one this comment used to give. The old reason is gone: the server no
     * longer replays a buffered offer at a rejoining receiver, and the phone
     * offers again every time it is told a peer has joined. What is left is
     * that there is nothing on the other end to recover to, because the phone
     * reads this same ICE failure as the end of the cast and stops its own
     * capture. Teach the sender to survive a failed connection and dropping the
     * socket here is what would put the picture back. From an idle source,
     * because this runs on a webrtcbin task thread and everything drop_session
     * touches belongs to the main one. */
    g_idle_add (drop_session_idle, self);
  }
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

  /* The record branch finishing, forwarded out of the bin by message-forward.
   *
   * record_drop_timer is the gate rather than the message source: it is armed
   * only between sending the branch its EOS and tearing it out, and the branch
   * is the only sub-bin in this pipeline that ever goes EOS while the session
   * runs. Inside that window this message can mean nothing else.
   *
   * Same thread as the timer it replaces: gst_bus_add_watch dispatches on the
   * default main context, so drop_record_branch still touches GTK from the
   * main thread. */
  if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ELEMENT && self->record_drop_timer) {
    const GstStructure *s = gst_message_get_structure (msg);
    GstMessage *forwarded = NULL;
    if (s && gst_structure_has_name (s, "GstBinForwarded")
        && gst_structure_get (s, "message", GST_TYPE_MESSAGE, &forwarded, NULL)) {
      gboolean eos = GST_MESSAGE_TYPE (forwarded) == GST_MESSAGE_EOS;
      gst_message_unref (forwarded);
      if (eos) {
        g_source_remove (self->record_drop_timer);
        drop_record_branch (self);
      }
    }
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

/* Watches the encoded stream for the two facts the repair needs: a loss has
 * happened, and a keyframe has since arrived.
 *
 * The order of the two tests is the whole of it -- the first buffer after a
 * flush is both DISCONT and an IDR, and that one is an arrival, not a hole.
 *
 * Runs on a streaming thread and the retry runs on the main one, hence the
 * atomic; it carries one bit and no ordering is needed beyond the bit itself. */
static GstPadProbeReturn
on_encoded_buffer (GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
  App *self = data;
  GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER (info);

  if (GST_BUFFER_FLAG_IS_SET (buf, GST_BUFFER_FLAG_DISCONT))
    g_atomic_int_set (&self->keyframe_wanted, 1);
  if (!GST_BUFFER_FLAG_IS_SET (buf, GST_BUFFER_FLAG_DELTA_UNIT))
    g_atomic_int_set (&self->keyframe_wanted, 0);

  return GST_PAD_PROBE_OK;
}

static gboolean
build_pipeline (App *self, JsonObject *turn)
{
  GStrv uris = turn_uris (turn);
  if (!uris && self->relay_only) {
    set_status (self, "The server sent no usable TURN URL");
    return FALSE;
  }

  self->pipeline = gst_pipeline_new ("kagami");
  /* A sink inside a bin does not post EOS to the pipeline bus: a pipeline
   * posts one EOS, and only once every sink it holds has seen it. The record
   * branch is a bin with a filesink in it, and its EOS is the one moment worth
   * knowing about -- it is when matroskamux has written its index and closed
   * the file. message-forward wraps that child message in a GstBinForwarded
   * element message, which is the documented way to watch a sub-bin finish
   * while the rest of the pipeline goes on playing. */
  g_object_set (self->pipeline, "message-forward", TRUE, NULL);
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
      /* The relay number is where webrtcbin starts, because it is the one that
       * is harmless to be wrong about: too much buffer on a direct pair costs
       * delay nobody dies of, too little on a relayed one costs the picture.
       * choose_latency() lowers it when the first pad shows the path is
       * direct, early enough that the sink is never told anything else. */
      "latency", self->latency_ms < 0 ? (guint) AIRCAST_LATENCY_RELAY
                                      : (guint) self->latency_ms,
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

typedef struct {
  App *self;
  gchar *text;
} Outgoing;

static gboolean
send_text (gpointer data)
{
  Outgoing *out = data;

  if (out->self->ws &&
      soup_websocket_connection_get_state (out->self->ws) == SOUP_WEBSOCKET_STATE_OPEN)
    soup_websocket_connection_send_text (out->self->ws, out->text);

  g_free (out->text);
  g_free (out);
  return G_SOURCE_REMOVE;
}

/* Two of send_json's three callers are not on the main thread: on_ice_candidate,
 * for every candidate webrtcbin trickles, and the answer, sent from the
 * create-answer promise. Both are webrtcbin task threads.
 * SoupWebsocketConnection belongs to the context it was created on -- its
 * outgoing frames are a bare GQueue with no lock and its output source is
 * attached there -- and on_ws_closed can clear self->ws on the main thread in
 * between the test and the send. So the text crosses over the way post_ui
 * already makes status text cross over, which also puts the state check next to
 * the send instead of a thread switch away from it. */
static void
send_json (App *self, JsonBuilder *builder)
{
  JsonGenerator *gen = json_generator_new ();
  JsonNode *root = json_builder_get_root (builder);
  json_generator_set_root (gen, root);
  gchar *text = json_generator_to_data (gen, NULL);

  Outgoing *out = g_new0 (Outgoing, 1);
  out->self = self;
  out->text = text;             /* ownership moves to send_text */
  g_idle_add (send_text, out);

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

  /* build_pipeline failed and on_message dropped its answer on the floor. Both
   * its failure paths leave webrtcbin NULL, and without this the two
   * g_signal_emit_by_name calls below are criticals on stderr while
   * "Negotiating" writes over the sentence that said what went wrong: a window
   * stuck for good on a word that is not true. */
  /* Second cast in one run of the program. "bye" and a failed ICE connection
   * both end the media session through drop_session without ending the
   * signalling socket, and "joined" -- the only caller of build_pipeline -- is
   * answered once per socket and is long past. Measured against the installed
   * build: an offer identical to one that drew an answer and 40 candidates
   * drew nothing at all once a "bye" had gone first, while the same double
   * "peer" without the bye still drew both, which is the control that puts the
   * cause on drop_session rather than on the repeated peer. Here rather than
   * on "peer" because every cast has to come through an offer, so one guard
   * covers bye, ICE failure and anything later that learns to call
   * drop_session. self->turn may be NULL, which is exactly what the first cast
   * passes when the server sends no TURN block.
   *
   * Tested on !pipeline rather than !webrtc so a build that fails after the
   * pipeline exists -- webrtcbin missing -- is not retried per offer. The
   * earlier failure, relay-only with no usable TURN URI, returns before the
   * pipeline is assigned and so is retried; it costs one overwritten status
   * line per offer and nothing else, which is the cheaper of the two wrongs. */
  if (!self->pipeline)
    build_pipeline (self, self->turn);

  if (!self->webrtc) {
    set_status (self, "No media pipeline, so this build cannot take the cast. "
        "See the terminal, and receiver/README.md");
    return;
  }

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
    JsonObject *turn = json_object_get_object_member (msg, "turn");
    /* Replaced rather than accumulated: every reconnect brings a fresh one,
     * and 110 reconnects were measured in one run of the leak test. */
    g_clear_pointer (&self->turn, json_object_unref);
    self->turn = turn ? json_object_ref (turn) : NULL;
    build_pipeline (self, turn);
  } else if (g_str_equal (kind, "peer")) {
    /* A new phone: an echo that never arrived must not swallow its bye. */
    self->disconnecting = FALSE;
    /* And the old one may still be here. A phone killed from Recents, out of
     * battery or force-stopped sends no bye, and the server does not say it
     * left, so the next phone's offer was set on the dead one's webrtcbin --
     * "set remote offer in the stable state", an answer carrying the old DTLS
     * certificate, the new phone stuck in ICE checking, and 14 s later the
     * failure that ended both. A remote description is what says a cast was
     * already negotiated here; a receiver that joined after the phone has
     * none yet, and keeps the pipeline "joined" has just built.
     *
     * A phone that rejoins without having died is covered too: it answers
     * "peer" with an ICE restart (sender/lib/session.dart _reoffer), which is
     * exactly what a fresh webrtcbin needs -- the receiver-rejoin case.
     *
     * ponytail: a recording in progress holds the old pipeline ~700 ms for its
     * index (drop_pending), and an offer inside that window still lands on the
     * old webrtcbin, so that one cast fails and the next press works. Stash the
     * offer until drop_record_branch finishes if that ever shows up. */
    GstWebRTCSessionDescription *remote = NULL;
    if (self->webrtc)
      g_object_get (self->webrtc, "remote-description", &remote, NULL);
    if (remote) {
      gst_webrtc_session_description_free (remote);
      g_message ("a new phone while the last cast was still up: dropping it");
      show_page (self, "idle");
      drop_session (self);
    }
    set_status (self, "A phone is connecting…");
  } else if (g_str_equal (kind, "offer")) {
    on_offer (self, json_object_get_string_member (msg, "sdp"));
  } else if (g_str_equal (kind, "candidate")) {
    JsonObject *c = json_object_get_object_member (msg, "candidate");
    g_signal_emit_by_name (self->webrtc, "add-ice-candidate",
        (guint) json_object_get_int_member_with_default (c, "sdpMLineIndex", 0),
        json_object_get_string_member (c, "candidate"));
  } else if (g_str_equal (kind, "bye")) {
    if (self->disconnecting) {
      /* The phone echoing our own Disconnect: the idle page already says
       * "Ready for the next cast" and the session is already gone. */
      self->disconnecting = FALSE;
    } else {
      stop_recording (self);
      show_page (self, "idle");
      set_status (self, "The phone stopped casting");
      /* And end it, rather than leaving a finished webrtcbin sending into a
       * transport the phone has already hung up. This runs on the main thread,
       * so it can call drop_session directly. */
      drop_session (self);
    }
  } else if (g_str_equal (kind, "error")) {
    /* Never render the server's own words: this label also carries the update
     * notice, and an attacker-controlled string in it is a phishing primitive. */
    g_message ("signalling error: %s",
        json_object_get_string_member_with_default (msg, "message", "(no detail)"));
    set_status (self, "Signalling error");
  }

  g_object_unref (parser);
}

/* The signalling socket is how a phone reaches this window, and losing it is
 * silent: the window looks exactly as it does when idle, the code on screen is
 * still the code the user is typing, and every cast they start from then on
 * goes nowhere. It happened on the first network change measured here. The
 * phone moved between access points, the ICE pair died, and 76 milliseconds
 * after the next join arrived the server logged "receiver left" -- after which
 * the program sat there, connected to nothing, looking healthy.
 *
 * So this reconnects rather than reporting. The delay doubles from one second
 * to thirty so a server that is down is not hammered, and the timer id is kept
 * because a close can arrive while one attempt is already pending.
 *
 * The dead pipeline goes with it. webrtcbin keeps trying to send on a
 * transport whose consent has been revoked -- the log fills with "Consent to
 * send has been revoked" every few seconds, indefinitely -- and the next cast
 * needs a clean webrtcbin anyway, since build_pipeline is what wires one up
 * from the TURN list the server hands out at join. */
static gboolean reconnect_signalling (gpointer data);

static void
drop_session (App *self)
{
  /* Before the pipeline goes: poll_stats asks webrtcbin for a report and would
   * otherwise fire once more against an element being torn down underneath it. */
  if (self->stats_timer) {
    g_source_remove (self->stats_timer);
    self->stats_timer = 0;
  }
  /* Blanked rather than frozen. A reading left on screen from a cast that
   * ended reads as a cast still running, which is the one thing the strip
   * exists to be honest about. */
  if (self->strip_path) {
    gtk_label_set_text (GTK_LABEL (self->strip_path), "\xe2\x80\x94");
    gtk_label_set_text (GTK_LABEL (self->strip_buffer), "\xe2\x80\x94");
    gtk_label_set_text (GTK_LABEL (self->strip_res), "\xe2\x80\x94");
    gtk_label_set_text (GTK_LABEL (self->strip_loss), "\xe2\x80\x94");
  }
  set_strip_state (self, NULL, "Waiting for a phone");

  if (self->recording)
    stop_recording (self);
  /* A recording still closing: the muxer has 700 ms to write its index and
   * needs the pipeline alive for them. Finish there instead -- drop_record_branch
   * calls back in -- rather than pull the pipeline out from under the file.
   * Disconnect used to close the window, and shutdown_app waits for the EOS. */
  if (self->record_branch) {
    self->drop_pending = TRUE;
    return;
  }
  if (self->pipeline) {
    /* The watch holds a ref on the bus and a GSource on the main context, and
     * build_pipeline installs a fresh one per session. Left behind, every
     * reconnect leaks both and the old handler keeps firing for a pipeline
     * nobody can see. */
    GstBus *bus = gst_element_get_bus (self->pipeline);
    gst_bus_remove_watch (bus);
    gst_object_unref (bus);
    gst_element_set_state (self->pipeline, GST_STATE_NULL);
    gst_object_unref (self->pipeline);
    self->pipeline = NULL;
    self->webrtc = NULL;
  }

  /* Everything the record button holds lived inside that pipeline. tee is a
   * borrowed pointer into the tail bin that has just been freed, record_branch
   * belongs to the pipeline, and record_tee_pad's ref outlives the tee that owns
   * the pad. stop_recording above does not finish the job either: it hands the
   * branch to a pad probe and a 700 ms timer, both of which expect the pipeline
   * to still be there. Cancelling that timer and clearing these pointers is what
   * stops a use-after-free on a bin that no longer exists, and what stops the
   * next press of the record button asking a freed tee for a pad -- a NULL check
   * on tee is all on_record_toggled has. */
  if (self->record_drop_timer) {
    g_source_remove (self->record_drop_timer);
    self->record_drop_timer = 0;
    set_status (self, "The recording was interrupted and may be incomplete");
  }
  self->record_branch = NULL;
  gst_clear_object (&self->record_tee_pad);
  self->tee = NULL;
  g_clear_pointer (&self->record_mux, g_free);
  self->latency_chosen = 0;
}

static void
on_ws_closed (SoupWebsocketConnection *ws, App *self)
{
  if (self->ws == ws) {
    g_clear_object (&self->ws);
    drop_session (self);
    /* After drop_session, which sets the neutral text: no phone can reach
     * this window until on_connected turns it back. */
    /* warn, not bad. The socket going is not the cast failing: the next
     * attempt is a second away and usually works, and painting it the same
     * colour as a failure told the user it was over when it was not. */
    set_strip_state (self, "warn", "Reconnecting to the server");
  }

  if (self->reconnect_source)
    return;

  if (self->reconnect_delay < 1)
    self->reconnect_delay = 1;
  post_ui (self, "The signalling connection closed. Reconnecting", "idle");
  self->reconnect_source =
      g_timeout_add_seconds (self->reconnect_delay, reconnect_signalling, self);
}

static void
on_connected (GObject *session, GAsyncResult *result, gpointer user_data);

/* One attempt per timer. The message has to be rebuilt: libsoup will not send
 * the same SoupMessage twice. */
static gboolean
reconnect_signalling (gpointer data)
{
  App *self = data;

  self->reconnect_source = 0;
  self->reconnect_delay = MIN (self->reconnect_delay * 2, 30);

  g_clear_object (&self->message);
  self->message = soup_message_new (SOUP_METHOD_GET, self->signal_url);
  soup_session_websocket_connect_async (self->session, self->message, NULL, NULL,
      G_PRIORITY_DEFAULT, NULL, on_connected, self);
  return G_SOURCE_REMOVE;
}

static void
on_connected (GObject *session, GAsyncResult *result, gpointer user_data)
{
  App *self = user_data;
  GError *error = NULL;

  self->ws = soup_session_websocket_connect_finish (SOUP_SESSION (session), result, &error);
  if (error) {
    gchar *msg = g_strdup_printf ("Cannot reach the signalling server: %s. Retrying",
        error->message);
    set_status (self, msg);
    g_free (msg);
    g_error_free (error);
    /* Arm the next attempt here too. A connect that fails never reaches
     * on_ws_closed, so without this the first failure would be the last and
     * the window would sit on the message for ever. */
    if (!self->reconnect_source) {
      if (self->reconnect_delay < 1)
        self->reconnect_delay = 1;
      self->reconnect_source = g_timeout_add_seconds (self->reconnect_delay,
          reconnect_signalling, self);
    }
    return;
  }

  /* Connected, so the next disconnection starts its backoff from one second
   * again rather than from wherever the last outage left it. */
  self->reconnect_delay = 1;

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

  /* Cleared, not set: the strip below says "Waiting for a phone" and the card
   * used to say it too, eight inches above it, in the one state the program
   * spends most of its life in. The strip is the surface that carries
   * connection state -- it is on both pages and it is never not there -- so
   * this line is left for what the strip cannot say: an error, or a transition
   * worth a sentence. Emptied rather than hidden, because a GtkLabel with no
   * text is still a line tall, and the card keeping its height means the next
   * message does not shove the footnote down when it arrives. */
  set_status (self, "");
  /* The strip, which until now said "Starting" from launch until the first
   * cast ended -- the one state it is never in once the socket is up. */
  set_strip_state (self, NULL, "Waiting for a phone");
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
    set_strip_state (self, "live", "Mirroring");
    /* One second, which is slow enough that get-stats costs nothing and fast
     * enough that a reading is never stale by the time it is read. The counters
     * restart with the session so the first interval is measured against this
     * cast and not the last one. */
    self->last_lost = self->last_recv = 0;
    if (!self->stats_timer)
      self->stats_timer = g_timeout_add_seconds (1, poll_stats, self);
  } else {
    set_status (self, "The video sink produced no paintable");
  }

  gst_object_unref (handover->sink);
  g_free (handover);
  return G_SOURCE_REMOVE;
}

/* The jitter buffer is sized from the path ICE actually got. The two paths want
 * numbers more than three times apart and only the program knows which one it
 * is holding, so the choice belongs here and not in a flag somebody has to
 * remember before double-clicking a shortcut.
 *
 * Both numbers were walked by hand on this deployment, one value per cast, and
 * what the logs hold is a gradient rather than a cliff. Each run states its own
 * value, as the gap between the new jitterbuffer and its first deadline
 * timeout: 60.1 ms in receiver12, 40.8 in receiver13, 29.7 in receiver14, 60.6
 * in receiver15, 197.7 in the relayed receiver10. Counted over the same 80
 * seconds after that jitterbuffer in each, the sink's "Have too many pending
 * frames" and the decoder's "Dropping frame due to QoS" do not follow the
 * number at all: 597 and 49 at 60 ms, 321 and 18 at 60 ms again on a longer
 * run, 372 and 37 at 40, 575 and 35 at 20. Two casts at the same 60 differ more
 * than 60 differs from 20, so those two counters are measuring this machine and
 * not the buffer. The one that does follow it is the H.264 decoder's "Invalid
 * frame num N, maybe frame drop", which is a real hole in the stream: 0 and 2
 * at 60 ms, 5 at 40, and 16 across the whole 20 ms run. Nothing here says
 * anything about lost packets, keyframe requests or discont, because the
 * jitterbuffer logs those at GST_DEBUG and every one of these runs was captured
 * at WARN, so a grep for them returns zero only because the category was never
 * recorded. What is known, then: 60 is the lowest value measured that cost no
 * visible frame gaps, the user could not tell it from 40, and 20 roughly
 * triples the gaps. That margin is nearly
 * free: the 20 ms it gives up against 40 is invisible inside a 70-80 ms
 * glass-to-glass budget, and the direct round trip it covers is 3 ms. The relay
 * keeps 200, for the reasons set out in main().
 *
 * Only a pair that is host at both ends gets the fast number. One relayed leg
 * is enough to make the path slow, the relay being 41 ms from the notebook and
 * 52 from the tablet, so both ends are read rather than just ours. A reflexive
 * pair is genuinely direct, but its round trip is whatever the internet between
 * the two peers happens to be and nothing here has measured that, so it keeps
 * the value that is safe to be wrong about.
 *
 * This signal is the first moment webrtcbin can answer the question at all, and
 * that is a fact about webrtcbin rather than about ICE. Its stats reach an ICE
 * transport only through a pad, and a receive-only session has no pad until
 * rtpbin hands one over: the receive pad is built when the answer is set and
 * then parked, under webrtcbin's own comment about delaying the pad until
 * rtpbin creates the recv output pad. Asking at connection-state CONNECTED asks
 * an element that has nothing to walk, and it answers with a report containing
 * no candidates. By the time this runs the pad exists and ICE settled long ago:
 * on the 60 ms run ICE reached completed 253 ms before the first jitterbuffer
 * was created, and this signal comes after that one. It cannot be otherwise,
 * since no RTP reaches rtpbin before ICE has a pair to carry it.
 *
 * The jitterbuffer therefore already exists here, which invites doing this from
 * the new-jitterbuffer handler above and skipping live reconfiguration
 * entirely. That deadlocks: rtpbin emits that signal from inside create_stream
 * with the session lock held, and webrtcbin's latency setter walks down to the
 * jitterbuffers behind the same lock. Setting webrtcbin's property rather than
 * the element's is also what makes one call cover the jitterbuffer that exists,
 * any a second stream would create, and rtpstorage's matching size.
 *
 * Nothing needs recalculating afterwards, because the tail that holds the sink
 * is built further down this same function: the sink is configured with the
 * chosen number the first time it is configured, and the picture never sees a
 * change. Moving the value once the sink is running is a different job, and if
 * the path changes under a live cast the number stays where it was. That is a
 * stutter, not a black window, and the cure is --latency or restarting. */
static void
choose_latency (App *self, GstPad *pad)
{
  GstPromise *promise;
  const GstStructure *reply = NULL;
  guint candidates = 0, host = 0;
  gint chosen;

  /* --latency is an override and turns the choice off. It doubles as the
   * fired-once guard: the number chosen below goes into the same field, so a
   * second pad finds the question already answered. */
  if (self->latency_ms >= 0)
    return;

  promise = gst_promise_new ();
  g_signal_emit_by_name (self->webrtc, "get-stats", pad, promise);
  if (gst_promise_wait (promise) == GST_PROMISE_RESULT_REPLIED)
    reply = gst_promise_get_reply (promise);

  /* A candidate appears in the report only as half of the pair ICE selected, so
   * counting them is the whole test and there is no need to follow
   * local-candidate-id and remote-candidate-id back to their structures.
   * candidate-type is libnice's own spelling, host, srflx, prflx or relay,
   * copied straight through. It is not the GstWebRTCICECandidateType nick,
   * which spells the same four types host, server-reflexive, peer-reflexive and
   * relayed. */
  for (gint i = 0; reply && i < gst_structure_n_fields (reply); i++) {
    const gchar *name = gst_structure_nth_field_name (reply, i);
    const GValue *value = gst_structure_get_value (reply, name);
    const gchar *type;

    if (!GST_VALUE_HOLDS_STRUCTURE (value))
      continue;
    type = gst_structure_get_string (gst_value_get_structure (value),
        "candidate-type");
    if (!type)
      continue;
    candidates++;
    if (g_str_equal (type, "host"))
      host++;
  }
  gst_promise_unref (promise);

  /* Every candidate host, and at least one of them, is the only shape that was
   * ever measured. Everything else takes the relay value, and so does
   * everything that went wrong: no reply, a reply carrying an error instead of
   * a report, or a transport with no selected pair yet. All of those count zero
   * and leave the number where build_pipeline put it. */
  /* The answer goes to webrtcbin and to latency_chosen, never back into
   * latency_ms, which holds only what the user typed. Writing it there would
   * make the guard above fire for the rest of the process, and a phone that
   * rejoins over the relay after a direct cast would keep 60 ms on a path whose
   * retransmissions need 142 ms at the median, which is the regime main()
   * describes below 150. build_pipeline runs once per process and a bye does
   * not tear the pipeline down, so that second cast is a real case. Asking
   * again per pad costs one more promise and is idempotent. */
  chosen = (candidates > 0 && host == candidates)
      ? AIRCAST_LATENCY_DIRECT : AIRCAST_LATENCY_RELAY;
  if (chosen == self->latency_chosen)
    return;
  self->latency_chosen = chosen;
  g_object_set (self->webrtc, "latency", (guint) chosen, NULL);
  /* One line on stderr, because this is now a number nobody typed and the
   * receiver's own log is the only place it can be read back. */
  g_printerr ("jitter buffer: %d ms (%s)\n", chosen,
      chosen == AIRCAST_LATENCY_DIRECT
          ? "ICE selected a host pair"
          : "relayed, reflexive or unknown path");
  /* Not written to the strip from here: this runs on the streaming thread
   * that delivered the pad, and a GtkLabel may only be touched from the main
   * one. apply_stats reads latency_chosen there and shows the buffer and the
   * path it was chosen by, within the second. */
}

/* One tail per incoming pad, chosen from the RTP caps. H.264 is what the phone
 * sends when it can; VP8 is the floor for devices libwebrtc does not consider
 * H.264-capable, and the two tails differ only in the first two elements. */
static void
on_pad_added (GstElement *webrtc, GstPad *pad, App *self)
{
  if (GST_PAD_DIRECTION (pad) != GST_PAD_SRC)
    return;

  choose_latency (self, pad);

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
   * max-lateness (gstbasesink.c:3126-3132; the 5 ms is gstvideosink.c:177, the
   * line below the 15 this paragraph opens with), some 22 ms further out. The
   * margin is a frame period plus those 5, so it read 38 while the sender was
   * capped at 30 and reads 22 now the cap is 60, and it moved because the frame
   * rate did rather than because anything here changed. 16.67 ms is its floor:
   * a buffer carrying no duration is given the running average of the
   * inter-frame gap instead, which is longer. The 227 was measured at 30 fps,
   * but what it measures is the network arriving late, and 12 ms clears 22. So
   * a couple of frames per half hour paint late instead of on the beat, and
   * none vanish. */
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

  /* The tee's sink pad, because it is the last point where the stream is still
   * the phone's own access units: h264parse has set DELTA_UNIT by then, and the
   * decoder below has not yet turned a damaged frame into a picture. */
  GstPad *watch = gst_element_get_static_pad (tee, "sink");
  g_atomic_int_set (&self->keyframe_wanted, 0);
  gst_pad_add_probe (watch, GST_PAD_PROBE_TYPE_BUFFER, on_encoded_buffer, self, NULL);
  gst_object_unref (watch);
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

/* The pairing QR, drawn rather than fetched: every byte of it is already in
 * this process.
 *
 * The payload carries the signalling URL as well as the code --
 * kagami://pair?c=<code>&s=<url> -- which is the whole reason it exists. A QR
 * of the six digits alone would save four seconds of typing; carrying the URL
 * is what lets one APK talk to whichever server the person in front of this
 * screen is running, instead of the one it was compiled against.
 *
 * Dark modules on a white plate, not inverted to match the card. An inverted
 * code is legal and most scanners read it, "most" being the problem: this is
 * the one thing on the screen whose whole job is to be read by a camera held
 * by someone who will blame the app, not their scanner.
 *
 * Whole-pixel modules. A fractional module size leaves a seam of background
 * between neighbours after antialiasing, and a scanner reading a 25-module
 * code off a screen has little enough contrast budget without it.
 */
static void
draw_pairing_qr (GtkDrawingArea *area, cairo_t *cr, int width, int height,
    gpointer data)
{
  QRcode *qr = g_object_get_data (G_OBJECT (area), "qr");
  if (!qr)
    return;

  /* Four modules, which is what the spec asks for and what a scanner needs to
   * find the code's edge against whatever is behind it. */
  const int quiet = 4;
  int span = qr->width + quiet * 2;
  int module = MAX (1, MIN (width, height) / span);
  int side = module * span;
  double ox = (width - side) / 2.0;
  double oy = (height - side) / 2.0;

  /* #eef2f6, and rounded. A pure white square with hard corners was the
   * brightest and squarest thing on a warm, round-cornered card, so it read as
   * pasted on rather than printed -- and it out-shouted the six digits, which
   * are what someone across the room is actually there to read. The radius is
   * two modules, which stays inside the four-module quiet zone and so takes
   * nothing a scanner needs. Against the ink below it this is still 16:1. */
  const double r = module * 2.0;
  cairo_new_sub_path (cr);
  cairo_arc (cr, ox + side - r, oy + r,        r, -G_PI / 2, 0);
  cairo_arc (cr, ox + side - r, oy + side - r, r, 0,         G_PI / 2);
  cairo_arc (cr, ox + r,        oy + side - r, r, G_PI / 2,  G_PI);
  cairo_arc (cr, ox + r,        oy + r,        r, G_PI,      3 * G_PI / 2);
  cairo_close_path (cr);
  cairo_set_source_rgb (cr, 0.933, 0.949, 0.965);
  cairo_fill (cr);

  /* #05080f, the window behind the card: the code reads as a hole cut in the
   * screen rather than as ink printed on it. */
  cairo_set_source_rgb (cr, 0.020, 0.031, 0.059);
  for (int y = 0; y < qr->width; y++) {
    for (int x = 0; x < qr->width; x++) {
      if (qr->data[y * qr->width + x] & 1)
        cairo_rectangle (cr, ox + (x + quiet) * module, oy + (y + quiet) * module,
            module, module);
    }
  }
  cairo_fill (cr);
}

static GtkWidget *
build_pairing_qr (App *self)
{
  GtkWidget *area = gtk_drawing_area_new ();
  gtk_widget_add_css_class (area, "qr");
  gtk_widget_set_halign (area, GTK_ALIGN_CENTER);
  /* A multiple of the module count plus its quiet zone, so the integer module
   * size above lands on this exactly rather than leaving a margin. */
  gtk_widget_set_size_request (area, 186, 186);

  /* Escaped, because the URL carries :// and a query of its own and this one is
   * a query value. */
  gchar *escaped = g_uri_escape_string (self->signal_url ? self->signal_url : "",
      NULL, FALSE);
  gchar *payload = g_strdup_printf ("kagami://pair?c=%s&s=%s", self->code, escaped);
  g_free (escaped);

  /* Level M: a quarter of the code can be lost and still read, which is the
   * level every phone camera pointed at a lit screen was tuned against.
   * Version 0 asks libqrencode for the smallest that fits, so the modules stay
   * as large as the payload allows. Case-sensitive, because the URL is. */
  QRcode *qr = QRcode_encodeString (payload, 0, QR_ECLEVEL_M, QR_MODE_8, 1);
  if (qr) {
    g_object_set_data_full (G_OBJECT (area), "qr", qr, (GDestroyNotify) QRcode_free);
  } else {
    /* An unencodable payload is not worth ending a cast over -- the six digits
     * below still pair. The blank square would be a lie, so the widget goes. */
    g_warning ("the pairing payload would not encode as a QR: %s", payload);
    gtk_widget_set_visible (area, FALSE);
  }
  g_free (payload);

  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (area), draw_pairing_qr, self, NULL);
  return area;
}

static GtkWidget *
build_idle_page (App *self)
{
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_halign (box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (box, "card");

  GtkWidget *title = gtk_label_new ("Kagami");
  gtk_widget_add_css_class (title, "title");

  GtkWidget *hint = gtk_label_new ("Scan this with Kagami on your phone, or type the code");
  gtk_widget_add_css_class (hint, "hint");

  /* Shown 482 913 rather than 482913. Six digits with nothing to break them
   * are counted twice by anyone reading them across a room -- once to find the
   * middle and once to keep the place -- and the group is the one thing the
   * eye can hold in a single look. Display only: self->code stays six
   * characters everywhere it is compared or sent, and a selection copied out
   * of the label carries a space the phone's field drops, because that field
   * takes digits and nothing else.
   *
   * Six is not assumed. Anything of another length is shown whole, since the
   * code can come from --code and there is no sensible middle of five. */
  gchar *shown = strlen (self->code) == 6
      ? g_strdup_printf ("%.3s %s", self->code, self->code + 3)
      : g_strdup (self->code);
  self->code_label = gtk_label_new (shown);
  g_free (shown);
  gtk_widget_add_css_class (self->code_label, "code");
  gtk_label_set_selectable (GTK_LABEL (self->code_label), TRUE);

  self->status_label = gtk_label_new ("Connecting to the signalling server…");
  gtk_widget_add_css_class (self->status_label, "status");
  gtk_label_set_wrap (GTK_LABEL (self->status_label), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (self->status_label), 48);
  gtk_label_set_justify (GTK_LABEL (self->status_label), GTK_JUSTIFY_CENTER);

  gtk_box_append (GTK_BOX (box), title);
  gtk_box_append (GTK_BOX (box), hint);
  gtk_box_append (GTK_BOX (box), build_pairing_qr (self));
  gtk_box_append (GTK_BOX (box), self->code_label);
  self->update_label = gtk_label_new ("");
  gtk_widget_add_css_class (self->update_label, "status");
  gtk_label_set_wrap (GTK_LABEL (self->update_label), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (self->update_label), 48);
  gtk_label_set_justify (GTK_LABEL (self->update_label), GTK_JUSTIFY_CENTER);
  gtk_label_set_selectable (GTK_LABEL (self->update_label), TRUE);

  gtk_box_append (GTK_BOX (box), self->status_label);

  /* Everything below the connection line is small print, and it used to be
   * four labels of nearly one size stacked eight pixels apart: the card ended
   * in a paragraph of grey that read as a program printing at the user. They
   * live in a footnote now -- one hairline, real space above it -- and the
   * update message sits on the same row as the button that acts on it rather
   * than on the line above it. */
  GtkWidget *footnote = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_add_css_class (footnote, "footnote");
  GtkWidget *update_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (update_row, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (update_row), self->update_label);

  /* Next to the label it writes to. It sat in the toolbar, which show_page
   * hides on this page -- so "click Update" pointed at nothing, and a press
   * during a cast reported to a page nobody could see. */
  GtkWidget *update = gtk_button_new_with_label ("Update");
  gtk_button_set_has_frame (GTK_BUTTON (update), FALSE);
  gtk_widget_add_css_class (update, "foot-link");
  gtk_widget_set_tooltip_text (update, "Check for a new version");
  g_signal_connect (update, "clicked", G_CALLBACK (on_update_clicked), self);
  gtk_box_append (GTK_BOX (update_row), update);
  gtk_box_append (GTK_BOX (footnote), update_row);

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
  gtk_widget_add_css_class (wireless, "foot-link");
  gtk_widget_set_tooltip_text (wireless,
      "Opens Settings > System > Projecting to this PC, where Windows' own "
      "Miracast receiver is installed and switched on. Adding it needs an "
      "administrator once. The picture is then Windows': aircast cannot record "
      "it or tune its latency.");
  g_signal_connect (wireless, "clicked", G_CALLBACK (on_wireless_display_clicked), self);
  gtk_box_append (GTK_BOX (footnote), wireless);
#endif

  gtk_box_append (GTK_BOX (box), footnote);
  return box;
}

static GtkWidget *
build_live_page (App *self)
{
  /* The bezel is the Reflector cue that this is a phone and not a window: a
   * thick dark rounded frame the video is clipped into. */
  GtkWidget *bezel = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (bezel, "bezel");

  self->picture = gtk_picture_new ();
  /* CONTAIN, and the bars it leaves are a decision the user made rather than
   * something nobody noticed. The tablet is 2304x1440 (1.60) and the notebook
   * 1920x1080 (1.78), so a correctly shaped picture leaves about 130 px down
   * each side even in fullscreen. COVER would fill the screen and is one
   * identifier away, but it fills it by cropping a tenth off the top and
   * bottom of the tablet's screen -- which takes the status bar with its clock
   * and battery, and the dock. Asked directly, the user kept the bars. */
  gtk_picture_set_content_fit (GTK_PICTURE (self->picture), GTK_CONTENT_FIT_CONTAIN);
  gtk_widget_set_hexpand (self->picture, TRUE);
  gtk_widget_set_vexpand (self->picture, TRUE);
  gtk_widget_add_css_class (self->picture, "screen");

  gtk_box_append (GTK_BOX (bezel), self->picture);

  /* No aspect frame around this, and that is a trade with a measured price.
   *
   * A GtkAspectFrame hugged the video so the bezel came out phone-shaped, and
   * it answered a real problem: a GtkPicture's natural size is the paintable's
   * own, so a centred parent capped the mirror at 1:1 and the hexpand above
   * did nothing. But an aspect frame measures height from width. The program
   * logged what that costs at a window 1100 px wide: "content wants at least
   * 721", which is (1100 - 92) / 1.6 plus the bezel's chrome. On a 2560 px
   * screen the same sum asks for about 1700, the window has 1392, and two
   * things follow from it.
   *
   * The toolbar and the connection strip are pushed past the bottom edge --
   * the strip sliced along the top of the taskbar, which is how this was
   * reported. And the window cannot be dragged smaller than a minimum taller
   * than the screen, so the pointer pulls at an edge and nothing moves.
   *
   * CONTENT_FIT_CONTAIN already letterboxes the video inside whatever box it
   * is given, and GtkPicture can shrink, so the bezel fills the area and asks
   * for nothing. The bezel stops being phone-shaped: it is the window's shape
   * now, with bars above and below the picture. On a 16:9 window showing a
   * 1.6 tablet those bars are about five percent, and .bezel (#03060b) and
   * .screen (#000000) are a shade apart, so what is lost is a silhouette
   * rather than a frame. A window that resizes is worth more. */
  gtk_widget_set_hexpand (bezel, TRUE);
  gtk_widget_set_vexpand (bezel, TRUE);
  return bezel;
}

/* The readings are PATH, BUFFER, PICTURE and LOSS, and every one of them is an
 * em dash until a session fills it. The idle card is the screen this program
 * spends most of its life showing, and four labelled dashes under a card asking
 * someone to scan a code are furniture rather than information -- so they
 * belong to the live page. The chevron goes with them: a fold control over
 * nothing to fold is worse than no control at all.
 *
 * The preference outlives the page, which is why it is a field and not the
 * widget's own visibility. Someone who folded the readings away mid-cast gets
 * them folded on the next one, instead of having the return to the idle card
 * quietly undo what they asked for. */
static void
apply_strip_readings (App *self)
{
  if (!self->strip_readings || !self->stack)
    return;
  const gchar *page = gtk_stack_get_visible_child_name (GTK_STACK (self->stack));
  gboolean live = page && g_str_equal (page, "live");
  gtk_widget_set_visible (self->strip_readings, live && self->readings_wanted);
  gtk_widget_set_visible (self->strip_toggle, live);
}

/* Folds the readings away, leaving the state and the chevron that brings them
 * back. The icon turns over with the state so the button says which way it
 * goes rather than what it is. */
static void
on_strip_toggled (GtkButton *button, App *self)
{
  const gchar *page = gtk_stack_get_visible_child_name (GTK_STACK (self->stack));
  /* D reaches here from the key handler on either page, and the chevron is not
   * on the idle one. Flipping a preference nobody can see flipped would have
   * the next cast open in a state its user never chose. */
  if (!page || !g_str_equal (page, "live"))
    return;

  self->readings_wanted = !self->readings_wanted;
  apply_strip_readings (self);
  gtk_button_set_icon_name (GTK_BUTTON (self->strip_toggle),
      self->readings_wanted ? "go-down-symbolic" : "go-up-symbolic");
  gtk_widget_set_tooltip_text (self->strip_toggle,
      self->readings_wanted ? "Hide the readings (D)" : "Show the readings (D)");
}

/* The strip's state half, driven from wherever the session's state actually
 * changes rather than polled. `css` is the beacon's colour class. */
static void
set_strip_state (App *self, const gchar *css, const gchar *text)
{
  if (!self->strip_dot)
    return;
  gtk_widget_remove_css_class (self->strip_dot, "live");
  gtk_widget_remove_css_class (self->strip_dot, "warn");
  gtk_widget_remove_css_class (self->strip_dot, "bad");
  if (css)
    gtk_widget_add_css_class (self->strip_dot, css);
  gtk_label_set_text (GTK_LABEL (self->strip_state), text);
}

/* Once a second while a session is up.
 *
 * webrtcbin's get-stats reply is a flat GstStructure of GstStructures, one per
 * RTCStats object, so the whole report is walked rather than indexed -- the
 * same shape choose_latency reads at pad-added, and the field names are the
 * ones the WebRTC statistics spec gives, lower-cased with hyphens.
 *
 * Asynchronous, and that is the whole reason this is three functions instead
 * of one. gst_promise_wait blocks its caller until webrtcbin answers, and
 * choose_latency can afford that because it runs once, on a streaming thread,
 * at pad-added. Here the caller would be the GTK main thread, once a second,
 * for the life of the cast -- the same thread that paints every frame. A
 * hundred milliseconds of it is six dropped frames, and this program has
 * already spent a day on one main-thread stall. So the reply is read on
 * webrtcbin's thread, reduced to six numbers, and those cross to the main
 * thread the way every other cross-thread update in this file does.
 *
 * Loss is reported per interval and not per session on purpose. A cast that
 * dropped a burst in its first ten seconds and has been clean for an hour
 * should not still show a bad number: the question is whether it is bad NOW. */
typedef struct {
  App *self;
  gdouble rtt_ms;               /* < 0 when the report carried none */
  guint width, height;
  guint64 lost, recv;
} Stats;

static gboolean
apply_stats (gpointer data)
{
  Stats *st = data;
  App *self = st->self;

  /* The session can end between the report being taken and this running. */
  if (!self->strip_path || !self->stats_timer) {
    g_free (st);
    return G_SOURCE_REMOVE;
  }

  if (self->latency_chosen > 0) {
    gchar *t = g_strdup_printf ("%d ms", self->latency_chosen);
    gtk_label_set_text (GTK_LABEL (self->strip_buffer), t);
    g_free (t);
    /* In the same words the buffer was chosen by: a relayed pair is the one
     * thing about a slow cast the user can act on, by moving both ends onto
     * the same network. */
    gtk_label_set_text (GTK_LABEL (self->strip_path),
        self->latency_chosen == AIRCAST_LATENCY_DIRECT ? "Direct" : "Relayed");
  }

  /* The paintable knows the picture's real size and needs no promise for it.
   * frame-width from the report is the encoder's view of the same thing and is
   * absent on some builds, so this is the reading that always has an answer --
   * and it is the one that explains a picture which starts small and grows. */
  if (!st->width && self->picture) {
    GdkPaintable *p = gtk_picture_get_paintable (GTK_PICTURE (self->picture));
    if (p) {
      st->width = (guint) gdk_paintable_get_intrinsic_width (p);
      st->height = (guint) gdk_paintable_get_intrinsic_height (p);
    }
  }
  if (st->width && st->height) {
    gchar *t = g_strdup_printf ("%u×%u", st->width, st->height);
    gtk_label_set_text (GTK_LABEL (self->strip_res), t);
    g_free (t);
  }

  if (st->recv >= self->last_recv && st->lost >= self->last_lost) {
    guint64 dl = st->lost - self->last_lost, dr = st->recv - self->last_recv;
    if (dr + dl > 0) {
      gchar *t = g_strdup_printf ("%.2f%%", 100.0 * (gdouble) dl / (gdouble) (dr + dl));
      gtk_label_set_text (GTK_LABEL (self->strip_loss), t);
      g_free (t);
    }
  }
  self->last_lost = st->lost;
  self->last_recv = st->recv;

  g_free (st);
  return G_SOURCE_REMOVE;
}

/* On a webrtcbin thread. Reads, reduces, hands over; touches no widget. */
static void
on_stats (GstPromise *promise, gpointer user_data)
{
  App *self = user_data;
  const GstStructure *reply = NULL;
  Stats *st;

  if (gst_promise_wait (promise) == GST_PROMISE_RESULT_REPLIED)
    reply = gst_promise_get_reply (promise);

  st = g_new0 (Stats, 1);
  st->self = self;
  st->rtt_ms = -1.0;

  for (gint i = 0; reply && i < gst_structure_n_fields (reply); i++) {
    const GValue *value = gst_structure_get_value (reply,
        gst_structure_nth_field_name (reply, i));
    const GstStructure *s;
    gdouble d;
    guint64 u;
    gint64 l;
    guint w;

    if (!GST_VALUE_HOLDS_STRUCTURE (value))
      continue;
    s = gst_value_get_structure (value);

    /* Seconds in the spec, milliseconds on a screen. */
    if (gst_structure_get_double (s, "round-trip-time", &d) && d >= 0.0)
      st->rtt_ms = d * 1000.0;
    /* G_TYPE_INT64 in gstwebrtcstats.c, signed because the spec lets duplicates
     * drive it negative; the uint64 getter refuses the type and read 0 for ever. */
    if (gst_structure_get_int64 (s, "packets-lost", &l) && l > 0)
      st->lost += (guint64) l;
    if (gst_structure_get_uint64 (s, "packets-received", &u))
      st->recv += u;
    if (gst_structure_get_uint (s, "frame-width", &w) && w) {
      st->width = w;
      gst_structure_get_uint (s, "frame-height", &st->height);
    }
  }

  gst_promise_unref (promise);
  g_idle_add (apply_stats, st);
}

static gboolean
poll_stats (gpointer data)
{
  App *self = data;
  GstPromise *promise;

  if (!self->webrtc) {
    self->stats_timer = 0;
    return G_SOURCE_REMOVE;
  }

  /* A picture broken by a loss stays broken, and nothing downstream notices.
   *
   * rtph264depay request-keyframe=true fires one PLI off the DISCONT the
   * jitterbuffer marks, and that is the whole of the recovery: if the IDR it
   * asks for is itself lost -- a quarter of a megabyte at 2304x1440, sent into
   * the same burst that caused the loss -- nothing asks again. The decoder goes
   * on painting P-frames against a reference it no longer holds, and on a
   * screen that has stopped changing the encoder sends nothing that would
   * overwrite the damage, so it sits there until the user does something
   * drastic. Netflix is the worst case of exactly this shape: minutes of
   * full-rate video, a burst of loss at the ceiling, and then a still launcher
   * whose wallpaper the encoder has no reason to code again.
   *
   * d3d11h264dec is what makes it visible rather than merely wrong.
   * avdec_h264 output-corrupt=false drops the wrecked frame; DXVA has no
   * equivalent and paints the reference surface's macroblocks into the holes.
   *
   * So the ask repeats until it is answered, once a second on the tick that was
   * already here. It costs one RTCP packet a second while the picture is
   * broken and nothing at all when it is not. */
  if (g_atomic_int_get (&self->keyframe_wanted))
    request_keyframe (self);

  /* NULL pad: the whole report rather than one transceiver's. The promise is
   * unreffed by on_stats, which the change func hands it to. */
  promise = gst_promise_new_with_change_func (on_stats, self, NULL);
  g_signal_emit_by_name (self->webrtc, "get-stats", NULL, promise);
  return G_SOURCE_CONTINUE;
}

/* One reading in the strip: a small upper-case key over a monospaced value.
 * Monospaced because these numbers change every second and a proportional font
 * makes the whole row twitch sideways as digits swap width. */
static GtkWidget *
strip_cell (const gchar *key, const gchar *initial, GtkWidget **value_out)
{
  GtkWidget *cell = gtk_box_new (GTK_ORIENTATION_VERTICAL, 1);
  gtk_widget_add_css_class (cell, "cell");

  GtkWidget *k = gtk_label_new (key);
  gtk_widget_add_css_class (k, "cell-key");
  gtk_widget_set_halign (k, GTK_ALIGN_START);

  GtkWidget *v = gtk_label_new (initial);
  gtk_widget_add_css_class (v, "cell-value");
  gtk_widget_set_halign (v, GTK_ALIGN_START);

  gtk_box_append (GTK_BOX (cell), k);
  gtk_box_append (GTK_BOX (cell), v);
  *value_out = v;
  return cell;
}

/* The strip along the bottom, on both pages and at all times.
 *
 * Everything in it was already known and none of it was shown. The path ICE
 * settled on decides the jitter buffer (choose_latency), the buffer decides a
 * third of the delay, and the resolution is what libwebrtc's quality scaler
 * happens to have left of the source -- which is the whole explanation for a
 * picture that starts small and grows, and it used to take a log file to see.
 * A row of six readings costs about fifty pixels of window and answers all of
 * it at a glance. */
static GtkWidget *
build_strip (App *self)
{
  GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (bar, "strip");

  GtkWidget *state = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class (state, "cell");
  /* The divider between the state and the readings belongs to the readings,
     not to the state: they are the half that comes and goes, and a rule left
     behind when they go is a row that looks cut off. */
  gtk_widget_add_css_class (state, "cell-first");
  /* A box, not an empty label: an empty GtkLabel is still one text line tall,
   * and the CSS disc came out as a pill. */
  self->strip_dot = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (self->strip_dot, "beacon");
  gtk_widget_set_valign (self->strip_dot, GTK_ALIGN_CENTER);
  self->strip_state = gtk_label_new ("Starting");
  gtk_widget_add_css_class (self->strip_state, "cell-state");
  gtk_box_append (GTK_BOX (state), self->strip_dot);
  gtk_box_append (GTK_BOX (state), self->strip_state);
  gtk_box_append (GTK_BOX (bar), state);

  /* The readings live in their own box so the whole group can be folded away
   * without taking the state with it. Hiding the strip outright would leave no
   * handle to bring it back and no answer to "am I still connected", which is
   * the one thing worth a permanent line of pixels. */
  self->strip_readings = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class (self->strip_readings, "readings");
  gtk_box_append (GTK_BOX (self->strip_readings), strip_cell ("PATH", "—", &self->strip_path));
  /* No LATENCY cell: a receive-only webrtcbin has no remote-inbound report and
   * so no round-trip-time; the phone's card shows it from its own stats. */
  gtk_box_append (GTK_BOX (self->strip_readings), strip_cell ("BUFFER", "—", &self->strip_buffer));
  gtk_box_append (GTK_BOX (self->strip_readings), strip_cell ("PICTURE", "—", &self->strip_res));
  gtk_box_append (GTK_BOX (self->strip_readings), strip_cell ("LOSS", "—", &self->strip_loss));
  gtk_box_append (GTK_BOX (bar), self->strip_readings);

  /* Eats the slack, so the readings stay left and do not spread out across a
   * wide window with a hand's width between them. */
  GtkWidget *spacer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand (spacer, TRUE);
  gtk_box_append (GTK_BOX (bar), spacer);

  self->strip_toggle = toolbar_button ("go-down-symbolic", "Hide the readings (D)");
  gtk_widget_add_css_class (self->strip_toggle, "strip-toggle");
  g_signal_connect (self->strip_toggle, "clicked", G_CALLBACK (on_strip_toggled), self);
  gtk_box_append (GTK_BOX (bar), self->strip_toggle);

  return bar;
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

  self->live_status = gtk_label_new ("");
  /* hint, not rec-time: rec-time is the recording indicator's alarm red and
   * "Saved" is not an alarm. */
  gtk_widget_add_css_class (self->live_status, "hint");
  /* MIDDLE, because the two messages worth reading here are file paths and
   * both ends of one matter. width_chars and max_width_chars both 40 so the
   * label is a FIXED size: it sits between the record timer and the buttons,
   * and one that grew with its text would slide every button sideways under
   * the cursor about to press one -- including Disconnect, two places along. */
  gtk_label_set_ellipsize (GTK_LABEL (self->live_status), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_width_chars (GTK_LABEL (self->live_status), 40);
  gtk_label_set_max_width_chars (GTK_LABEL (self->live_status), 40);

  GtkWidget *fullscreen = toolbar_button ("view-fullscreen-symbolic", "Fullscreen (F)");
  self->fullscreen_button = fullscreen;
  g_signal_connect (fullscreen, "clicked", G_CALLBACK (on_fullscreen_clicked), self);

  GtkWidget *quit = toolbar_button ("window-close-symbolic", "Disconnect");
  g_signal_connect (quit, "clicked", G_CALLBACK (on_disconnect_clicked), self);


  gtk_box_append (GTK_BOX (bar), self->record_button);
  gtk_box_append (GTK_BOX (bar), self->record_time);
  gtk_box_append (GTK_BOX (bar), self->live_status);
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

  /* GtkApplication is single-instance: a second launch on a machine with a
   * session bus does not start a second process, it re-emits activate here.
   * Everything below writes into one App struct, so running it twice repoints
   * the live window's status line and record button at a second window's
   * widgets, and opens a second signalling socket on a code the server has
   * already handed us -- which it refuses, and the receiver then loops on that
   * refusal. The window we already have is the whole answer. */
  if (self->window) {
    gtk_window_present (GTK_WINDOW (self->window));
    return;
  }

  load_css ();
  /* A selectable GtkLabel selects all its text when it takes keyboard focus,
   * and the pairing code is the first focusable widget in the window -- so the
   * code opened painted as one blue selection block. Mouse selection and
   * copying still work with this off. */
  g_object_set (gtk_settings_get_default (), "gtk-label-select-on-focus", FALSE, NULL);

  self->window = gtk_application_window_new (app);
  gtk_window_set_title (GTK_WINDOW (self->window), "Kagami");
  /* By name, not by file: GTK looks this up in the icon theme, which on
   * Windows is the hicolor tree tools/bundle-windows.sh lays down beside the
   * exe and indexes. The exe's own resource icon is what Explorer and the
   * taskbar read; this is the one GTK draws inside the window, and a build
   * with no theme installed simply has no icon rather than an error. */
  gtk_window_set_icon_name (GTK_WINDOW (self->window), "kagami");
  gtk_window_set_default_size (GTK_WINDOW (self->window), 1100, 760);
  gtk_widget_add_css_class (self->window, "room");

  self->stack = gtk_stack_new ();
  gtk_stack_set_transition_type (GTK_STACK (self->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_add_named (GTK_STACK (self->stack), build_idle_page (self), "idle");
  gtk_stack_add_named (GTK_STACK (self->stack), build_live_page (self), "live");

  /* Stacked rather than overlaid. The toolbar used to float over the video and
   * appear on mouse movement, which hid the button that ends the cast at
   * exactly the moment someone reaches for it -- they have been looking at
   * their phone's screen, not moving a mouse. It sits under the picture now,
   * and the strip under that, both in the window's own vertical box. The
   * fifty-odd pixels that costs come out of a picture that is letterboxed
   * against a 16:9 monitor anyway.
   *
   * vexpand on the stack alone is what keeps the two bars at the bottom: a box
   * gives its spare height to whichever child asks for it. */
  GtkWidget *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_vexpand (self->stack, TRUE);
  self->toolbar = build_toolbar (self);
  /* Hidden until there is something to operate. On the idle page every button
   * in it is either meaningless or destructive. */
  gtk_widget_set_visible (self->toolbar, FALSE);
  self->strip = build_strip (self);
  self->strip_was_shown = TRUE;
  /* The window opens on the idle page without going through show_page -- the
   * stack shows whichever child was added first -- so the first application of
   * the rule above has to happen here. */
  apply_strip_readings (self);
  gtk_box_append (GTK_BOX (column), self->stack);
  gtk_box_append (GTK_BOX (column), self->toolbar);
  gtk_box_append (GTK_BOX (column), self->strip);
  g_signal_connect (self->window, "notify::fullscreened",
      G_CALLBACK (on_fullscreen_changed), self);
  gtk_window_set_child (GTK_WINDOW (self->window), column);

  GtkEventController *keys = gtk_event_controller_key_new ();
  g_signal_connect (keys, "key-pressed", G_CALLBACK (on_key_pressed), self);
  gtk_widget_add_controller (self->window, keys);

  gtk_window_present (GTK_WINDOW (self->window));
  mark ("window presented");

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

  /* Before anything else: a pending reconnect would otherwise fire into a
   * half-torn-down app. */
  if (self->reconnect_source) {
    g_source_remove (self->reconnect_source);
    self->reconnect_source = 0;
  }

  if (self->recording)
    stop_recording (self);
  /* Only from OPEN. A close the peer began leaves the connection in CLOSING
   * with our echo already queued and the "closed" signal not yet emitted, and
   * calling close() again there is the libsoup CRITICAL
   * "assertion '!priv->close_sent' failed" that receiver16.err.log caught on
   * quit. The assertion returns without sending anything, so the tidy close we
   * came here for is the one thing that does not happen. */
  if (self->ws &&
      soup_websocket_connection_get_state (self->ws) == SOUP_WEBSOCKET_STATE_OPEN)
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
  /* The kept TURN block. One reference, replaced on every "joined". */
  g_clear_pointer (&self->turn, json_object_unref);
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
   * All of that is the relayed path, and it is no longer the only path this
   * program sees. -1 means nobody has chosen yet: build_pipeline starts
   * webrtcbin on the relay number above, because that is the one that is safe
   * to be wrong about, and choose_latency() drops it to
   * AIRCAST_LATENCY_DIRECT when the first pad shows a pair that is host at
   * both ends. The same walk on a direct pair bottoms out between 20 and 40 ms
   * rather than at 200, which is a different curve entirely: there is no relay
   * to cross and no retransmission worth waiting three round trips for.
   *
   * --latency overrides both numbers and turns the choice off, and it is still
   * worth re-walking: the run that made 120 ms stutter predates every fix
   * since. */
  /* G_MININT, not -1: -1 is a number the user can type, and the guards below
   * read this field as "negative means nobody has chosen". Anything reachable
   * from the command line has to stay out of the sentinel's way. */
  App self = { .latency_ms = G_MININT, .readings_wanted = TRUE };

  startup_us = g_get_monotonic_time ();
  /* First statement in main(): before gst_init() runs inside the option parse,
   * and before anything can cache a data directory.
   *
   * The flag is read from argv rather than from the parse below for the same
   * reason: gst_init runs inside g_option_context_parse, and GST_REGISTRY has
   * to be set before it. */
  gboolean prebuild = FALSE;
  for (int i = 1; i < argc; i++)
    if (g_str_equal (argv[i], "--prebuild-registry"))
      prebuild = TRUE;
  harden_environment (prebuild);

  GOptionEntry entries[] = {
    { "signal", 's', 0, G_OPTION_ARG_STRING, &self.signal_url,
        "Signalling server URL (default: " AIRCAST_SIGNAL ")", "URL" },
    { "code", 'c', 0, G_OPTION_ARG_STRING, &self.code,
        "6-digit pairing code (generated and shown if omitted)", "CODE" },
    { "record-dir", 'r', 0, G_OPTION_ARG_FILENAME, &self.record_dir,
        "Where the record button writes .mkv files (default: Videos)", "DIR" },
    { "latency", 'l', 0, G_OPTION_ARG_INT, &self.latency_ms,
        "Jitter buffer in ms, and turns off the automatic choice. Left out, "
        "the path chooses: " G_STRINGIFY (AIRCAST_LATENCY_DIRECT) " on a "
        "direct pair, " G_STRINGIFY (AIRCAST_LATENCY_RELAY) " on a relayed "
        "one", "MS" },
    { "insecure", 0, 0, G_OPTION_ARG_NONE, &self.insecure,
        "Allow a plaintext ws:// signalling URL. LAN bring-up only", NULL },
    { "selftest", 0, 0, G_OPTION_ARG_NONE, &self.selftest,
        "Run the version and signature self-tests and exit", NULL },
    /* Hidden: the installer's, not the user's. Nothing to type and nothing to
       get wrong -- gst_init has already written the registry by the time the
       flag is read, so this only has to not open a window. */
    { "prebuild-registry", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE,
        &self.prebuild_registry,
        "Build the shared plugin registry and exit", NULL },
    { "verify-manifest", 0, 0, G_OPTION_ARG_FILENAME, &self.verify_manifest,
        "Verify an update manifest against this build's key and exit", "FILE" },
    { "verify-signature", 0, 0, G_OPTION_ARG_FILENAME, &self.verify_signature,
        "The .minisig for --verify-manifest", "FILE" },
    { "relay-only", 0, 0, G_OPTION_ARG_NONE, &self.relay_only,
        "Send everything through the TURN relay, never directly. Slower, and "
        "the sender needs AIRCAST_RELAY=true to match", NULL },
    { NULL },
  };

  /* Somewhere for the output to go, now that this is a GUI-subsystem binary
   * (receiver/CMakeLists.txt) and double-clicking it no longer opens a console
   * beside the window. A GUI process starts with no console attached at all,
   * so stderr is a closed handle and every g_printerr and every GStreamer
   * warning would be thrown away -- including the ones that diagnosed every
   * problem this project has had.
   *
   * Two cases, and the first is why this is not simply a log file. Started
   * from a terminal, AttachConsole borrows the parent's console and the output
   * appears there exactly as it did before the subsystem changed, so
   * `kagami --help` and a debugging run still work. Started from the
   * shell or a shortcut there is no parent console, and stderr goes to
   * receiver.log under the user's data directory, truncated each run so it is
   * the last session rather than a year of them. stdout is left alone in that
   * case: nothing reads it, and pointing two FILE streams with two buffers at
   * one file interleaves them into nonsense. */
#ifdef G_OS_WIN32
  /* Read before AttachConsole, which may replace them: a handle the caller
   * redirected (2> file) is kept, only an unset one is pointed at the console. */
  HANDLE out0 = GetStdHandle (STD_OUTPUT_HANDLE);
  HANDLE err0 = GetStdHandle (STD_ERROR_HANDLE);
  gboolean out_set = out0 && out0 != INVALID_HANDLE_VALUE;
  gboolean err_set = err0 && err0 != INVALID_HANDLE_VALUE;
  if (AttachConsole (ATTACH_PARENT_PROCESS)) {
    if (!out_set)
      freopen ("CONOUT$", "w", stdout);
    if (!err_set)
      freopen ("CONOUT$", "w", stderr);
  } else if (err_set) {
    /* No console but stderr was handed to us: leave it where it points. */
  } else {
    gchar *dir = g_build_filename (g_get_user_data_dir (), "aircast", NULL);
    g_mkdir_with_parents (dir, 0700);
    gchar *path = g_build_filename (dir, "receiver.log", NULL);
    if (freopen (path, "w", stderr))
      /* Line buffered, so a crash still leaves the line that preceded it. */
      setvbuf (stderr, NULL, _IOLBF, 0);
    g_free (path);
    g_free (dir);
  }
#endif

  GOptionContext *ctx = g_option_context_new ("- Kagami");
  g_option_context_add_main_entries (ctx, entries, NULL);
  g_option_context_add_group (ctx, gst_init_get_option_group ());
  GError *error = NULL;
  mark ("options built");
  /* gst_init runs inside this, and with it the plugin registry: 299 plugins
   * in the shipped bundle, every one of them a LoadLibrary. */
  if (!g_option_context_parse (ctx, &argc, &argv, &error)) {
    g_printerr ("%s\n", error->message);
    return 1;
  }
  g_option_context_free (ctx);
  mark ("gst_init done");

  /* gst_init wrote the registry on its way through the parse. */
  if (self.prebuild_registry)
    return 0;

  /* Refused out loud rather than quietly reverting to the automatic choice the
   * flag was typed to turn off. Zero goes with the negatives: a jitter buffer
   * with no window calls every reordered packet lost, and the run these numbers
   * come from reordered 8,234 of 18,065. */
  if (self.latency_ms != G_MININT && self.latency_ms < 1) {
    g_printerr ("--latency must be a positive number of milliseconds\n");
    return 1;
  }

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

  /* Without this every frame is scaled and blitted by the CPU. GTK 4.24 leaves
   * Direct Composition off by default on Windows, and gdk/win32 makes both the
   * GL and the Vulkan context require it unconditionally
   * (gdkglcontext-win32.c:81, gdkvulkancontext-win32.c:133, both asking
   * gdk_win32_display_get_dcomp_device), so the renderer falls all the way back
   * to GskCairoRenderer -- confirmed on the shipped binary, which printed
   * "OpenGL requires Direct Composition" and then "Using renderer
   * 'GskCairoRenderer'" until this line existed. The upstream comment
   * (gdkdisplay-win32.c:511) says the opt-in is about black borders under GL
   * and that the default flips back when the D3D12 renderer lands.
   *
   * What it costs to leave alone is not small and not linear. Measured against
   * the bundle's own libcairo with a real 2304x1440 frame: 3.8 ms when the
   * window happens to be at 1:1 or an exact half, but 31.7 ms in the default
   * 1100x760 window and 47 ms at 0.75 -- the sink appends a plain
   * GskTextureNode without setting a filter, so cairo downscales with
   * FILTER_GOOD, a separable convolution. 31.7 ms of main-thread paint against
   * a 16.67 ms frame is a hard 32 fps ceiling, and it is where the warning
   * "Have too many pending frames" comes from:
   * that warning is gtk4paintablesink's 3-slot channel to the GTK main thread
   * overflowing, not the jitter buffer running short.
   *
   * Set rather than overridden, so GDK_DEBUG from the environment still wins;
   * gdk_pre_parse reads it with g_getenv at gtk_init time, which is why it has
   * to be here and not in harden_environment's neighbourhood by accident.
   *
   * And on Windows only where a D3D11 device can actually be made, because on a
   * machine where one cannot this flag is not free. gdk_win32_display_init_dcomp
   * (gdk/win32/gdkdisplay-win32.c:512 in 4.24.0) returns early while the flag is
   * off, and otherwise goes straight to
   *   hr_warn (ID3D11Device_QueryInterface (self->d3d11_device, ...));
   * with no null test -- and hr_warn only logs. self->d3d11_device is NULL both
   * when gdk_win32_display_init_d3d fails outright, which its call site at :783
   * logs and walks past, and when it succeeds through either of the two
   * D3D12-only terms of its chain at :544, which pass NULL for the D3D11 slot.
   * Measured on the shipped bundle: GDK_DISABLE=d3d11,d3d12 segfaults inside
   * gtk_init, and so does GDK_DISABLE=d3d11 alone, where init_d3d returns TRUE
   * and logs nothing. With the flag suppressed the same run reaches
   * "Using renderer 'GskCairoRenderer'" and mirrors, slowly. Probed through
   * GetProcAddress so nothing new is linked; BGRA_SUPPORT and
   * hardware-before-WARP are what gdk_win32_display_create_d3d_devices asks for.
   * This is a proxy for GTK's own adapter walk, not the same test. */
#ifdef G_OS_WIN32
  {
    HMODULE d3d11 = LoadLibraryW (L"d3d11.dll");
    PFN_D3D11_CREATE_DEVICE create = d3d11
        ? (PFN_D3D11_CREATE_DEVICE) (void *) GetProcAddress (d3d11, "D3D11CreateDevice")
        : NULL;
    ID3D11Device *probe = NULL;
    if (create &&
        (SUCCEEDED (create (NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
                            D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
                            D3D11_SDK_VERSION, &probe, NULL, NULL)) ||
         SUCCEEDED (create (NULL, D3D_DRIVER_TYPE_WARP, NULL,
                            D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
                            D3D11_SDK_VERSION, &probe, NULL, NULL))))
      {
        ID3D11Device_Release (probe);
        g_setenv ("GDK_DEBUG", "dcomp", FALSE);
      }
    else
      g_message ("no Direct3D 11 device: painting on the CPU");
    if (d3d11)
      FreeLibrary (d3d11);
  }
#else
  g_setenv ("GDK_DEBUG", "dcomp", FALSE);
#endif

  mark ("the Direct3D probe done");
  gtk_init ();
  mark ("gtk_init done");

  self.app = gtk_application_new ("io.kagami.receiver", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect (self.app, "activate", G_CALLBACK (activate), &self);
  g_signal_connect (self.app, "shutdown", G_CALLBACK (shutdown_app), &self);
  /* GtkApplication would otherwise try to parse our own arguments again. */
  int status = g_application_run (G_APPLICATION (self.app), 0, NULL);

  g_object_unref (self.app);
  return status;
}
