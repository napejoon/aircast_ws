/* The receiver's whole look, as one C string included by load_css().
 *
 * Kept as string literals rather than a .css file so the binary needs no data
 * files installed next to it — on Windows especially, "where is style.css"
 * is a worse problem than escaped newlines.
 *
 * Kagami is a mirror, and the room it draws is a moon over deep water. Three
 * colours, each with a job:
 *
 *   #e8edf2  moonlight. The pairing code, and the QR plate it sits under.
 *            The brightest thing on the screen, because it is the one thing
 *            somebody has to read from the other side of a room.
 *   #6fe3c4  the green the sea goes where the light lands. Live: the beacon,
 *            the focus ring, the halo under the plate. Never text.
 *   #ffb35c  amber. Recording, and the hotter #ff7a5c for a link that has
 *            dropped. Warm against a window that is otherwise entirely cold,
 *            which is what makes it findable without being shrill.
 *
 * Amber says "recording" where a red would shout it. That is the one
 * deliberate risk in this sheet: the mark is easier to miss. It is paired with
 * a filled button and a running timer for that reason -- the colour is not
 * carrying the state on its own.
 *
 * The ground is #05080f rather than black. A pure black window behind a card
 * that is nearly black gives the card no edge to be found by, and the gradient
 * gives the room a direction a flat fill never had.
 */

/* The gradient runs bottom-up, lighter at the top, because the card sits in
 * the upper middle of the window and needs the contrast where it is. */
"window.room {"
"  background: linear-gradient(to top, #04060c 0%, #05080f 45%, #0a1424 100%);"
"}"

/* ------------------------------------------------------------- idle card */

/* It was #131b1d on #0b1214: three percent of lightness between the card and
 * the room, which at a normal viewing distance is no edge at all, and the
 * shadow was doing the whole job of saying an object was there. Now the fill
 * is lifted, the border is a real hairline with some colour in it, and the
 * inset highlight along the top edge is the one pixel that makes a flat
 * rectangle read as a raised surface. */
".card {"
"  padding: 44px 60px 36px 60px;"
"  border-radius: 28px;"
"  background: linear-gradient(to bottom, #111f33 0%, #0b1526 100%);"
"  border: 1px solid #22344f;"
"  box-shadow: 0 32px 80px rgba(0, 0, 0, 0.62), inset 0 1px 0 rgba(255, 255, 255, 0.05);"
"}"

".title {"
"  font-size: 13px;"
"  font-weight: 700;"
"  letter-spacing: 5px;"
"  text-transform: uppercase;"
/* Dimmed moonlight, not the green. The green means live, and the head of this
 * file says so -- then the rename swept every turquoise into it, wordmark
 * included, and the card came back with its brightest colour spent on saying
 * its own name. A six-digit code someone is squinting at across a room is not
 * the place to spend contrast on identity, and neither is the word above it. */
"  color: #9fb3c8;"
"  margin-bottom: 4px;"
"}"

/* The instruction, and the only sentence on the card anyone has to read before
 * they know what to do with it. Moonlight dimmed one step: under the code,
 * clear of the footnotes, and still a colour and not a grey. */
".hint {"
"  font-size: 15px;"
"  color: #9fb3c8;"
"  margin-bottom: 22px;"
"}"

/* The code is the one thing a user has to read across a room, so it is the
 * brightest and the largest thing on it. Full moonlight, which on this ground
 * is the only place the palette spends all of its light. */
".code {"
"  font-size: 72px;"
"  font-weight: 800;"
"  letter-spacing: 16px;"
"  font-feature-settings: 'tnum' 1;"
"  color: #e8edf2;"
"  margin: 20px 0 6px 0;"
/* Letter-spacing is applied after every character including the last, so the
 * label is 16 px wider on the right than the digits are, and a centred label
 * sits 8 px left of where it looks like it should. The padding puts the gap
 * back on the other side. */
"  padding-left: 16px;"
"}"

/* The plate is drawn by the widget, so all this owns is the space around it
 * and the halo under it. The halo is the sea green at a tenth: a bright plate
 * on a dark card had a hard cut-out edge that made it read as pasted on rather
 * than lit, and this is the cheapest way to sit it in the same room as
 * everything else. */
".qr {"
"  margin: 6px 0 4px 0;"
"  box-shadow: 0 0 44px rgba(111, 227, 196, 0.10);"
"}"

/* Connection state, and the first of the small lines under the code. */
".status {"
"  font-size: 12px;"
"  color: #7b8ea6;"
"}"

/* The four small grey lines under the code used to be four separate labels of
 * nearly one size, stacked eight pixels apart, and the whole group read as a
 * program printing at the user rather than as a card. The footnote is what
 * they sit in now: a hairline, real space above it, and everything inside it
 * one size smaller than the line that matters. */
".footnote {"
"  margin-top: 20px;"
"  padding-top: 16px;"
"  border-top: 1px solid #1a2b45;"
"}"

/* The way out of this window, and the update check beside it: text that can be
 * clicked, not buttons that look like they run the cast. The sea green on
 * hover only, so the resting card stays a card and not a page of links.
 *
 * Not called "link". That is GtkLinkButton's own style class, Adwaita defines
 * button.link with an underline and the accent colour, and a class named after
 * what a thing is rather than where it lives collides with the stock sheet:
 * the first build of this card came back with both footnotes underlined. */
"button.foot-link {"
"  padding: 4px 8px;"
"  border: none;"
"  border-radius: 8px;"
"  background: transparent;"
"  font-size: 12px;"
"  color: #7b8ea6;"
"}"

"button.foot-link:hover {"
"  background: rgba(111, 227, 196, 0.08);"
"  color: #6fe3c4;"
"}"

"button.foot-link:focus-visible {"
"  outline: 2px solid #6fe3c4;"
"  outline-offset: -2px;"
"  color: #6fe3c4;"
"}"

/* ---------------------------------------------------------- mirrored screen */

/* A phone-shaped frame: thick dark bezel, generous corner radius, and a soft
 * drop shadow so the screen reads as an object in a room rather than a video
 * pinned to a wall. */
".bezel {"
"  padding: 14px;"
"  margin: 32px;"
"  border-radius: 34px;"
"  background: #03060b;"
"  border: 1px solid #22344f;"
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
"  background: #070d18;"
"  border-top: 1px solid #1a2b45;"
"}"

".cell {"
"  padding: 8px 16px;"
"  border-right: 1px solid #1a2b45;"
"}"

/* The readings sit in their own box (build_strip), so this is the last of them
 * and not the last thing in the strip. A rule with nothing to its right reads
 * as a row that was cut off rather than one that ended. */
".cell:last-child {"
"  border-right: none;"
"}"

/* Which is exactly what the state cell became once the readings stopped
 * appearing on the idle page: a label, a rule, and nothing after it. The
 * divider is the readings' left edge now, so it leaves when they do. */
".cell-first {"
"  border-right: none;"
"}"

".readings {"
"  border-left: 1px solid #1a2b45;"
"}"

".cell-key {"
"  font-size: 9px;"
"  font-weight: 700;"
"  letter-spacing: 2px;"
"  color: #5d718c;"
"}"

/* Monospaced and tabular: these numbers change every second, and in a
 * proportional face the whole row twitches sideways as digits swap width. */
".cell-value {"
"  font-family: monospace;"
"  font-size: 12px;"
"  font-feature-settings: 'tnum' 1;"
"  color: #e8edf2;"
"}"

".cell-state {"
"  font-size: 12px;"
"  color: #9fb3c8;"
"}"

/* A disc drawn by the box rather than by a glyph, so it does not depend on
 * whatever font happens to carry a filled circle. Grey until there is a
 * session to be green about. */
".beacon {"
"  min-width: 8px;"
"  min-height: 8px;"
"  border-radius: 999px;"
"  background: #41546e;"
"}"

/* The halo, not a bigger disc: the strip is 8 px of colour against the frame
 * and the eye skips it. A ring at a sixth of the same colour is visible from
 * the far side of a desk and takes no room the row has to give up, because a
 * box-shadow is drawn outside the layout. */
".beacon.live {"
"  background: #6fe3c4;"
"  box-shadow: 0 0 0 3px rgba(111, 227, 196, 0.16);"
"}"

/* Amber for the state between working and failed. A link that dropped and is
 * being retried is neither, and it had no colour of its own: it borrowed the
 * failure red and said the cast was over a second before it came back. Warm,
 * so it is told apart from the green beside it by hue and not only by
 * lightness -- which is what a red-green colourblind viewer has. */
".beacon.warn {"
"  background: #ffb35c;"
"  box-shadow: 0 0 0 3px rgba(255, 179, 92, 0.16);"
"}"

".beacon.bad {"
"  background: #ff7a5c;"
"  box-shadow: 0 0 0 3px rgba(255, 122, 92, 0.16);"
"}"

/* ---------------------------------------------------------------- toolbar */

/* A row under the picture, not a pill floating over it. It used to appear on
 * mouse movement and fade out again, which hid the button that ends the cast
 * at the moment someone reaches for it: they have been watching a phone
 * screen, not moving a mouse. No radius and no shadow, because it is part of
 * the window's frame now rather than an object sitting on the video. */
".toolbar {"
"  padding: 6px 10px;"
"  background: #070d18;"
"  border-top: 1px solid #1a2b45;"
"}"

"button.tool {"
"  min-width: 38px;"
"  min-height: 38px;"
"  padding: 0;"
"  border-radius: 999px;"
"  border: none;"
"  background: transparent;"
"  color: #9fb3c8;"
"}"

"button.tool:hover {"
"  background: #182842;"
"  color: #ffffff;"
"}"

/* F, R and D drive these from the keyboard, so Tab reaching one has to show.
 * The same fill as hover plus an outline, because the fill alone is the state
 * the pointer already uses and would not say which button holds focus. */
"button.tool:focus-visible {"
"  background: #182842;"
"  color: #ffffff;"
"  outline: 2px solid #6fe3c4;"
"  outline-offset: -2px;"
"}"

/* Recording is loud on purpose — a session recorded by accident is the one
 * mistake this program can make that the user cannot undo. Amber filled, with
 * the ground's own colour for the glyph: a dark mark on a bright button is
 * read at a glance as a state, where bright-on-dark is read as one more
 * button. */
"button.tool.recording {"
"  background: #ffb35c;"
"  color: #05080f;"
"}"

"button.tool.recording:hover {"
"  background: #ffc57e;"
"  color: #05080f;"
"}"

".rec-time {"
"  padding: 0 10px;"
"  font-size: 13px;"
"  font-feature-settings: 'tnum' 1;"
"  color: #ffb35c;"
"}"

/* The chevron that folds the readings away. Quieter than a toolbar button:
 * it is a preference, not an action on the cast. */
"button.strip-toggle {"
"  min-width: 26px;"
"  min-height: 26px;"
"  margin: 0 6px;"
"  color: #5d718c;"
"}"

"button.strip-toggle:hover {"
"  background: #182842;"
"  color: #9fb3c8;"
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
