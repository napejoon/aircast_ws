/* The receiver's whole look, as one C string included by load_css().
 *
 * Kept as string literals rather than a .css file so the binary needs no data
 * files installed next to it — on Windows especially, "where is style.css"
 * is a worse problem than escaped newlines.
 */

"window.room {"
"  background: #0b1214;"
"}"

/* ------------------------------------------------------------- idle card */

".card {"
"  padding: 40px 56px;"
"  border-radius: 22px;"
"  background: #131b1d;"
"  border: 1px solid #1f2b2d;"
"  box-shadow: 0 24px 64px rgba(0, 0, 0, 0.55);"
"}"

".title {"
"  font-size: 15px;"
"  font-weight: 600;"
"  letter-spacing: 3px;"
"  text-transform: uppercase;"
/* The wordmark is the one place the brand colour belongs on a card whose job
 * is to be read across a room: the code below it stays the brightest thing
 * there, because a six-digit code someone is squinting at is not the place to
 * spend contrast on identity. */
"  color: #3ddcd0;"
"  margin-bottom: 18px;"
"}"

".hint {"
"  font-size: 14px;"
"  color: #96acad;"
"}"

/* The code is the one thing a user has to read across a room. */
".code {"
"  font-size: 64px;"
"  font-weight: 700;"
"  letter-spacing: 14px;"
"  font-feature-settings: 'tnum' 1;"
"  color: #eff7f7;"
"  margin: 14px 0 8px 0;"
/* Letter-spacing is applied after every character including the last, so the
 * label is 14 px wider on the right than the digits are, and a centred label
 * sits 7 px left of where it looks like it should. The padding puts the gap
 * back on the other side. */
"  padding-left: 14px;"
"}"

".status {"
"  font-size: 12px;"
"  color: #6d8384;"
"}"

/* ---------------------------------------------------------- mirrored screen */

/* A phone-shaped frame: thick dark bezel, generous corner radius, and a soft"
 * drop shadow so the screen reads as an object in a room rather than a video"
 * pinned to a wall. */
".bezel {"
"  padding: 14px;"
"  margin: 32px;"
"  border-radius: 34px;"
"  background: #04090a;"
"  border: 1px solid #273334;"
"  box-shadow: 0 30px 80px rgba(0, 0, 0, 0.7);"
"}"

/* No minimum size. It was 320x240, and with the aspect frame in front of it
 * that made GTK's own layout invariant fail: the frame derives its ratio from
 * this child, and 240 px plus the bezel's 94 px of chrome came back as a
 * natural height of 334 against a minimum of 335 -- one pixel of ratio
 * rounding, and "natural size must be >= min size" on every measure with no
 * paintable attached. The frame is what stops the video collapsing now, which
 * is the whole job the minimum was doing. */
".screen {"
"  border-radius: 22px;"
"  background: #000000;"
"}"

/* ------------------------------------------------------- connection strip */

/* Along the bottom on both pages. Reads as part of the window's frame rather
 * than as a card floating in it, so it has one hairline above it and no
 * radius, no shadow and no fill of its own beyond a shade off the room. */
".strip {"
"  background: #0e1618;"
"  border-top: 1px solid #1f2c2e;"
"}"

".cell {"
"  padding: 8px 16px;"
"  border-right: 1px solid #1f2c2e;"
"}"

/* The readings sit in their own box (build_strip), so this is the last of them
 * and not the last thing in the strip. A rule with nothing to its right reads
 * as a row that was cut off rather than one that ended. */
".cell:last-child {"
"  border-right: none;"
"}"

".cell-key {"
"  font-size: 9px;"
"  font-weight: 700;"
"  letter-spacing: 2px;"
"  color: #4A5F60;"
"}"

/* Monospaced and tabular: these numbers change every second, and in a
 * proportional face the whole row twitches sideways as digits swap width. */
".cell-value {"
"  font-family: monospace;"
"  font-size: 12px;"
"  font-feature-settings: 'tnum' 1;"
"  color: #eff7f7;"
"}"

".cell-state {"
"  font-size: 12px;"
"  color: #c7dadb;"
"}"

/* A disc drawn by the box rather than by a glyph, so it does not depend on
 * whatever font happens to carry a filled circle. Grey until there is a
 * session to be green about. */
".beacon {"
"  min-width: 8px;"
"  min-height: 8px;"
"  border-radius: 999px;"
"  background: #4A5F60;"
"}"

/* The halo, not a bigger disc: the strip is 8 px of colour against #0e1618 and
 * the eye skips it. A ring at a sixth of the same colour is visible from the
 * far side of a desk and takes no room the row has to give up, because a
 * box-shadow is drawn outside the layout. */
".beacon.live {"
"  background: #3ddcd0;"
"  box-shadow: 0 0 0 3px rgba(61, 220, 208, 0.16);"
"}"

/* Amber for the state between working and failed. A link that dropped and is
 * being retried is neither, and it had no colour of its own: it borrowed the
 * failure red and said the cast was over a second before it came back. */
".beacon.warn {"
"  background: #ffc857;"
"  box-shadow: 0 0 0 3px rgba(255, 200, 87, 0.16);"
"}"

".beacon.bad {"
"  background: #ff6b5e;"
"  box-shadow: 0 0 0 3px rgba(255, 107, 94, 0.16);"
"}"

/* ---------------------------------------------------------------- toolbar */

/* A row under the picture, not a pill floating over it. It used to appear on
 * mouse movement and fade out again, which hid the button that ends the cast
 * at the moment someone reaches for it: they have been watching a phone
 * screen, not moving a mouse. No radius and no shadow, because it is part of
 * the window's frame now rather than an object sitting on the video. */
".toolbar {"
"  padding: 6px 10px;"
"  background: #0e1618;"
"  border-top: 1px solid #1f2c2e;"
"}"

"button.tool {"
"  min-width: 38px;"
"  min-height: 38px;"
"  padding: 0;"
"  border-radius: 999px;"
"  border: none;"
"  background: transparent;"
"  color: #c7dadb;"
"}"

"button.tool:hover {"
"  background: #212f30;"
"  color: #ffffff;"
"}"

/* F, R and D drive these from the keyboard, so Tab reaching one has to show.
 * The same fill as hover plus an outline, because the fill alone is the state
 * the pointer already uses and would not say which button holds focus. */
"button.tool:focus-visible {"
"  background: #212f30;"
"  color: #ffffff;"
"  outline: 2px solid #3ddcd0;"
"  outline-offset: -2px;"
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
"  color: #4A5F60;"
"}"

"button.strip-toggle:hover {"
"  background: #192426;"
"  color: #c7dadb;"
"}"

/* Fullscreen is the mirror and nothing else: the frame that makes this read as
 * an object in a room is exactly what wastes a screen when the screen is all
 * there is. */
"window.immersive .bezel {"
"  margin: 0;"
"  padding: 0;"
"  border-radius: 0;"
"  border-width: 0;"
"  box-shadow: none;"
"}"

"window.immersive .screen {"
"  border-radius: 0;"
"}"

"window.immersive .toolbar {"
"  border-top-width: 0;"
"}"
