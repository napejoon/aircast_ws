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

/* ------------------------------------------------------- connection strip */

/* Along the bottom on both pages. Reads as part of the window's frame rather
 * than as a card floating in it, so it has one hairline above it and no
 * radius, no shadow and no fill of its own beyond a shade off the room. */
".strip {"
"  background: #101317;"
"  border-top: 1px solid #232830;"
"}"

".cell {"
"  padding: 8px 16px;"
"  border-right: 1px solid #232830;"
"}"

".cell-key {"
"  font-size: 9px;"
"  font-weight: 700;"
"  letter-spacing: 2px;"
"  color: #4E5661;"
"}"

/* Monospaced and tabular: these numbers change every second, and in a
 * proportional face the whole row twitches sideways as digits swap width. */
".cell-value {"
"  font-family: monospace;"
"  font-size: 12px;"
"  font-feature-settings: 'tnum' 1;"
"  color: #f2f5f9;"
"}"

".cell-state {"
"  font-size: 12px;"
"  color: #c9d1dc;"
"}"

/* A disc drawn by the box rather than by a glyph, so it does not depend on
 * whatever font happens to carry a filled circle. Grey until there is a
 * session to be green about. */
".beacon {"
"  min-width: 8px;"
"  min-height: 8px;"
"  border-radius: 999px;"
"  background: #4E5661;"
"}"

".beacon.live {"
"  background: #3ddc91;"
"}"

".beacon.bad {"
"  background: #ff6b5e;"
"}"

/* ---------------------------------------------------------------- toolbar */

/* A row under the picture, not a pill floating over it. It used to appear on
 * mouse movement and fade out again, which hid the button that ends the cast
 * at the moment someone reaches for it: they have been watching a phone
 * screen, not moving a mouse. No radius and no shadow, because it is part of
 * the window's frame now rather than an object sitting on the video. */
".toolbar {"
"  padding: 6px 10px;"
"  background: #101317;"
"  border-top: 1px solid #232830;"
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

/* The chevron that folds the readings away. Quieter than a toolbar button:
 * it is a preference, not an action on the cast. */
"button.strip-toggle {"
"  min-width: 26px;"
"  min-height: 26px;"
"  margin: 0 6px;"
"  color: #4E5661;"
"}"

"button.strip-toggle:hover {"
"  background: #1B1F26;"
"  color: #c9d1dc;"
"}"
