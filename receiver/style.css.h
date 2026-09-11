/* The receiver's whole look, as one C string included by load_css().
 *
 * Kept as string literals rather than a .css file so the binary needs no data
 * files installed next to it — on Windows especially, "where is style.css"
 * is a worse problem than escaped newlines.
 */

"window.room {"
"  background: #0d0f12;"
"}"

/* ------------------------------------------------------------- idle card */

".card {"
"  padding: 40px 56px;"
"  border-radius: 22px;"
"  background: #15181d;"
"  border: 1px solid #23272e;"
"  box-shadow: 0 24px 64px rgba(0, 0, 0, 0.55);"
"}"

".title {"
"  font-size: 15px;"
"  font-weight: 600;"
"  letter-spacing: 3px;"
"  text-transform: uppercase;"
"  color: #6f7784;"
"  margin-bottom: 18px;"
"}"

".hint {"
"  font-size: 14px;"
"  color: #98a1ae;"
"}"

/* The code is the one thing a user has to read across a room. */
".code {"
"  font-size: 64px;"
"  font-weight: 700;"
"  letter-spacing: 14px;"
"  font-feature-settings: 'tnum' 1;"
"  color: #f2f5f9;"
"  margin: 14px 0 8px 0;"
"}"

".status {"
"  font-size: 12px;"
"  color: #6f7784;"
"}"

/* ---------------------------------------------------------- mirrored screen */

/* A phone-shaped frame: thick dark bezel, generous corner radius, and a soft"
 * drop shadow so the screen reads as an object in a room rather than a video"
 * pinned to a wall. */
".bezel {"
"  padding: 14px;"
"  margin: 32px;"
"  border-radius: 34px;"
"  background: #05070a;"
"  border: 1px solid #2b3038;"
"  box-shadow: 0 30px 80px rgba(0, 0, 0, 0.7);"
"}"

".screen {"
"  border-radius: 22px;"
"  min-width: 320px;"
"  min-height: 240px;"
"  background: #000000;"
"}"

/* ---------------------------------------------------------------- toolbar */

/* Floats over the video, appears on movement, fades out again. */
".toolbar {"
"  padding: 8px;"
"  border-radius: 999px;"
"  background: rgba(20, 23, 28, 0.92);"
"  border: 1px solid #2b3038;"
"  box-shadow: 0 12px 32px rgba(0, 0, 0, 0.6);"
"}"

"button.tool {"
"  min-width: 38px;"
"  min-height: 38px;"
"  padding: 0;"
"  border-radius: 999px;"
"  border: none;"
"  background: transparent;"
"  color: #c9d1dc;"
"}"

"button.tool:hover {"
"  background: #242a33;"
"  color: #ffffff;"
"}"

/* Recording is loud on purpose — a session recorded by accident is the one"
 * mistake this program can make that the user cannot undo. */
"button.tool.recording {"
"  background: #c0392b;"
"  color: #ffffff;"
"}"

"button.tool.recording:hover {"
"  background: #d0453a;"
"}"

".rec-time {"
"  padding: 0 10px;"
"  font-size: 13px;"
"  font-feature-settings: 'tnum' 1;"
"  color: #ff6b5e;"
"}"
