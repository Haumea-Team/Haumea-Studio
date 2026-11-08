# Changelog

## Unreleased: Changes since 1.0.0.242 beta

### Editor interface and performance

- Simplify the toolbar, welcome screen and Object Info, with dark and light themes,
  dockable panels, visible virtual world and interior fields, and an FPS indicator.
- Browse by feature: Objects, Characters, Paths, Markers, Text & audio, Zones,
  Prefabs, Favourites and Model files. Character and path tools have a setup panel
  before placement; Keep placing repeats placement until Esc.
- Change an object's model ID in Object Info, with undo and redo.
- Set one preview stream distance for every object in Preferences → Performance,
  independent of model, weather and time. Distance fog softens the cutoff;
  VSync and an optional frame limit control presentation and FPS.
- Add Simplified Chinese, bundle its Noto Sans SC font, and update Chinese and
  Portuguese translations for the redesigned controls.
- Make right-click menus tolerate small cursor movements without starting camera look.

### Mapping tools and races

- Pick from 29 more themes in Preferences (Catppuccin, Dracula, Gruvbox, Nord, Solarized,
  Tokyo Night and more) besides Dark and Light.
- Open maps whose arguments are arithmetic or named numbers: `BASE_Z + 2.5`, numeric
  `#define`s and `const`s, hex.
- Filter models that explode (object.dat). Map Check notes explosive objects and warns
  about trees and palms tilted on X or Y, which the game stands upright.
- Selected and hovered objects show their outline from the camera too, so smooth models
  such as barrels and rocks have a contour.
- Paths and curves: per-point handle modes (Automatic, Aligned, Free, Corner) and tilt,
  a start and end range, side and height offsets, and spacing by count. Loops and helices
  can fit the selected model's length. The Array tool repeats a line in rows for a grid.
- Moving objects: MoveObject / MoveDynamicObject on an object, kept relative to it, with an
  optional turn, a preview of the movement in the 3D view, prefabs, Map Check and Namaka keys.
- Animations a skin can't use are dimmed in the picker, with a Map Check warning: 46 move
  only an object's parts, 34 twist the skin.
- The Texture Browser filters textures that tile (left to right, top to bottom or both),
  and Namaka's texture search marks them.
- Duplicate offset in Tools: Ctrl+D moves and turns each copy by a fixed or random amount,
  in world or the object's own axes (also Namaka's map_tool duplicate).
- Races: point to point or circuits with laps, automatic checkpoint types (Finish at the end),
  a walk-through preview in the editor, and playable races for `haumea_races.inc`
  (StartMapRace, laps and times, with or without the streamer; `/race` in Test in game).

### Installation and workspaces

- Install Haumea with the Windows or Linux installer, or the unsigned Apple Silicon DMG.
  Each platform also has one portable download, with Namaka included.
- Switch between **Map**, **TextDraws** and **Code** in a single top bar. Yellow **New!**
  hints introduce TextDraws and Code and stay dismissed after use.
- Edit Pawn in the native **Code** workspace with syntax colours, line numbers, undo/redo,
  find/replace and recoverable drafts. Manual changes save to a separate script.
- Discord Rich Presence shows the active workspace and session time, with a switch under
  Preferences → Privacy. Map names and project paths stay private.
- Disabling Discord activity or closing Haumea waits briefly for the clear acknowledgement,
  preventing an unread reply from discarding the request.

### Map documents

- Saves now create clean Pawn code and a companion XML with the full editable scene: objects,
  materials, groups, notes, hidden state, radar layers, TextDraw layers and guides, and alignment pins.
- Older maps migrate when saved, keeping an exact backup of their previous Pawn. Paired saves
  retain the previous document if writing fails; externally edited Pawn keeps its runtime changes.

### Multiplayer editing

- Edit one map together: **File → Multiplayer...** hosts the open map, and others join it
  with the address, port and session code. Changes to every element, removed GTA buildings
  and the map's textdraws show up for everyone, with each person's camera and selection.
- `haumea-server` keeps a session on a machine that stays up (the SA-MP server's, say), with a
  password, and in a state file across restarts; the first editor in sends its map. Hosting from
  the editor asks the router to forward the port (UPnP) and shows a public IPv6 address when there
  is one; the server listens on IPv6 and IPv4 and journals every change, so a crash loses nothing.
  Connections are encrypted (Monocypher).
- Area removals, radar art, the TextDraw editor's file, alignment pins, 3D paths, the Nodes tool's
  NPC nodes and 0.3.DL custom models (their files included) travel too. Undo skips steps on what someone else changed since.
- Several agents build in one session, in one editor or several: each has its own edit session,
  and rolling back or undoing only takes back its own edits (Namaka's `multiplayer_session`).
- Others' markers move smoothly and show where they look (a view cone), selections have thick
  outlines, and an arrow at the edge of the view points to who is out of it.
- Chat in the Multiplayer panel: @name mentions, a toast for new messages, and double-click a
  message to look from where it was written. haumea-server keeps the last 200 messages.
- AI agents carry an **AI** tag everywhere: their marker, the People list and their messages
  (the server puts it there, so it can't be left off).
- The Join panel remembers the servers you joined; star favourites, optionally with their password.
- haumea-server keeps the map as Pawn (`--maps-dir`, with dated copies, and `--map-out`, for your
  game server's folder) whenever someone saves, and autosaves it (`--autosave`, every 10 minutes it
  changed). **Load a map for everyone** replaces the session's map; `--admin-password` limits who may.
- Servers with accounts, invites, roles (owner, admin, builder, viewer) and rooms: sign in from the
  Join panel, pick a room from the lobby, manage people from the People list and the Admin section.
  haumea-server is now its own project (github.com/Haumea-Team/Haumea-Server).
- The server proves who it is: Haumea remembers its fingerprint and refuses another one later.
- Everyone's ping shows beside their name; an AI agent shows whose it is ("via ...").
- Namaka joins the session your editor is in by itself (with a token from your account on a server
  with accounts), instead of asking you for the address and password.
- Fixed: the camera could roll over upside down when looking past straight up or down.
- Fixed: crashes when someone else changed an animated actor or NPC.
- Namaka works off your screen by default: its first look at the scene starts a hidden editor that
  joins your map as a person of its own, so your camera and selection stay yours. `namaka_team` starts
  several, and any tool takes `namaka` to work as one of them, or `namaka="editor"` to work where you
  watch. If you're in no session, one opens for this computer only.
- A terminal panel (**Windows → Terminal**, Ctrl+\`): your own shell inside Haumea, with colours, full-screen
  programs and scrollback, so any AI assistant's command-line tool runs there, or a server or anything else.
- New Namaka tools: `redo_edit`, `select_elements`, `add_path_node`, `path_nodes_status`.

### TextDraw editing and previews

- Artboards, layers, multiselection, locking, editable shapes, eight resize handles,
  arrangement tools, rulers and guides with exact positions, margins and layout presets.
- Zoom up to 1600% and pan over the canvas and textdraws. Browse or drop images, and use
  custom-model TXD sprites (`mdl-<id>:<texture>`, 0.3.DL).
- Preview GTA's HUD and SA-MP chat, including Widescreen Fix layout and `/hudscalefix`.
  A pixel-exact canvas renders at the player's resolution before scaling for editing.
- Model previews now match the game's placement, orientation and mirroring more closely,
  including planes, skins and negative preview sizes.
- Textdraws without a name of their own are written as one array per kind,
  `<prefix>_td[k]` and `<prefix>_ptd[playerid][k]`, instead of one variable each.
  Named textdraws keep their own variables, and older files read back into the arrays.

### Finding objects and measuring

- **Find instances** in the Object Browser accepts multiple model IDs and ranges, shows
  per-model counts, and filters your map, GTA's world or both. Results are yellow and
  camera jumps take you between locations and interiors without changing the selection.
- **Similar models** compares collision-box dimensions or proportions with adjustable
  tolerance; the existing previews let you compare the models' actual shapes.
- **Coordinate pins and alignment guides** in Tools: multiple movable pins and endpoints,
  object surface and anchor snapping, linked rulers, axis/angle modifiers and exact XYZ,
  distance, heading and pitch. Objects snap to the guides, align by centres or edges, or
  distribute between the ends, with undo.

### Updating from an older beta

Older Linux builds may reopen the previous executable after installing an update. If the version
stays unchanged, close Haumea completely and launch it again from its original installation or
shortcut. The new build fixes subsequent restarts.

### Fixes and polish

- Typing a GTA building's own coordinate back into Object Info, as it is shown, no longer counts
  as an edit (the game's 13.6484375 is written 13.648438).
- Windows code generation uses the user's temporary folder, fixing Pawn and TextDraw
  generation failures that required running the editor as administrator.
- Linux self-updates launch the newly installed executable rather than the old rollback copy.
- Missing essential GTA files return to the folder chooser with the missing names listed.
- macOS requests the required OpenGL Core profile; a failed graphics initialization stops
  startup instead of continuing into a crash. Windows installer verification waits for the
  installed executable to finish and checks its actual exit code.
- Fix the Windows prefab-placement stack overflow, browser flickering and a missing world
  frame after clicks or Namaka commands.
- Keep the first selected object as the transform pivot and preserve selection highlights
  across LODs. Small world props remain visible farther away.
- Apply the swap interval only when it changes and limit rendering while unfocused, addressing
  the reported background memory-growth path. NVIDIA and native Windows still need confirmation.
- Prefer open.mp's launcher when available, improve camera flight controls and initial window
  placement, and update English and Brazilian Portuguese documentation and labels.

## 1.0.0: Haumea Studio

The first release of **Haumea Studio, the all-in-one editor for open.mp and
SA-MP**. It edits whole open.mp and SA-MP maps, not just objects: NPCs and
their routes, vehicles with their tuning, pickups, labels, sounds,
checkpoints and textdraws, all saved as Pawn that compiles cleanly. It runs
from its own folder, tests your map on a real server with one key, and tells
you what will break before you find out in game. (Haumea Studio grew out of
the Haumea map editor; what changed for its users is in
[docs/HISTORY.md](docs/HISTORY.md).)

### New

**Installation and editors**
- Windows and Linux installers, an unsigned Apple Silicon DMG and one complete portable download
  per platform. Each includes Namaka; separate companion ZIPs are no longer needed.
- **Map**, **TextDraws** and **Code** share the top bar. TextDraws and Code have yellow discovery
  hints that stay dismissed after first use or dismissal.
- A native **Code** editor for Pawn with syntax highlighting, line numbers, undo/redo,
  find/replace and recoverable drafts. Manual edits save as a separate script.
- Incomplete GTA installations stay on the folder chooser with the missing files listed.
- Linux self-updates relaunch the updated installation, preserving the original launch path
  when the running executable moves into the rollback backup.


**open.mp NPCs**
- Place NPCs with a name, skin, weapon, health and armour, and pick how they
  move: along a path you draw from path points (`NPC_MoveByPath`), on the
  game's path nodes (`NPC_PlayNode`), or by playing a `.rec` recording
  (`NPC_StartPlayback`). Paths can loop.
- **Preview movement** walks them in the viewport at open.mp's speeds, and
  recordings play back on a timeline with their route drawn.
- Put an NPC in a vehicle, as driver or passenger; driver recordings move the
  car.
- Actors and NPCs can play an animation, and ped skins animate in the
  viewport.
- **Nodes** window (F3): add, link, move and delete the path nodes NPCs walk
  (`NODES*.DAT`) and save them to the server's `scriptfiles/NPCs/nodes`. The
  game's own node files are never changed.

**More map elements**
- Vehicles get SA-MP's 256 colours, a number plate, a paintjob and one tuning
  part per slot (only parts that fit), shown on the model.
- Trains stand on the track as SA-MP puts them, with the carriages the game adds
  (three behind a Freight or a Brown Streak), and slide along it when dragged.
- Sound places play an audio stream or a game sound to players nearby.
- Checkpoints and race checkpoints (streamer maps), with race order.
- **TextDraw designer**: draw global and per-player textdraws on the 640x448
  canvas, with the game's fonts, sprites and model previews. Textdraws have a
  `.pwn`/`.inc` file of their own (New, Open, Save, recent files in the
  TextDraws window), which Test in game includes; the map holds only the
  world. **TextDraw mode** swaps the world for a plain canvas or a still frame
  and turns map editing off, with its own undo; Esc leaves it. Maps from
  earlier builds keep their textdraws read-only until you move them to a file.
- SA-MP 0.3.DL and open.mp **custom models** from the server's
  `models/artconfig.txt`, under Custom models in the browser.
- Open and merge **Texture Studio** maps (`.db`), **MTA:SA** `.map` files and
  IPL text maps.

**Testing your map**
- **Test in game** (F5): runs the map as it is on a private open.mp server
  and shows its log. Your map file and your own server aren't touched.
- **Map Check** (F6): what will break or look wrong in game, before you save:
  limits (objects, vehicles, pickups, labels, actors, gang zones, map icons,
  textdraws, NPC slots), unknown models, positions outside the world,
  material and colour-code mistakes, invalid NPC names, missing recordings,
  paths with one point, duplicate variable names, and what the server needs.
  Click an item to select what it's about. It also runs, quietly, on save and
  Test in game.
- **Player view** (P): see the map the way a player would from the camera,
  with draw and stream distances, the 1000-object limit, and only the current
  virtual world and interior.

**Editing**
- **Transform tools** in the Tools window: array (linear or around a centre),
  mirror, align and distribute, exact moves and turns, and objects laid along
  a line, each one undo step.
- **Radar art from objects**: gang zones drawn from the objects' footprints,
  coloured from their textures, by group or in one colour, with a zone
  counter and "fit to N". Gang zones can be global, per-player or streamed
  around each player (open.mp only).
- **Hide and isolate** for busy areas: H hides the selection, Shift+H hides
  everything else, Alt+H shows it all. Only the editor hides them; the map
  keeps them.
- **Map Objects** is an outliner for big maps: groups, search, rename (F2),
  group the selection (Ctrl+G), and an eye to hide each object.
- **Texture Browser** (Ctrl+T) searches every texture.
- **Interiors** panel with the game's interiors and their spawn points.
- Duplicate (Ctrl+D) and focus the camera on the selection (F).
- **Edit → Go to Coordinates** (Ctrl+J) takes a pasted position (`x, y, z`,
  `x y z` with an angle after it, or a `SetPlayerPos` line) and points the
  camera there; **Copy Camera Position** copies it back as `x, y, z`.
- Namaka can move your view when you ask it to show you something (`move_camera`, a short
  fly-to; `camera_back` returns), never while you're dragging, placing or looking around.
- Every keyboard shortcut can be rebound in **Preferences → Shortcuts**, and
  every mouse-wheel action has keys: Ctrl+= / Ctrl+- / Ctrl+0 for zoom (and
  numpad + / -), Ctrl+Shift+= / - for fly speed, Ctrl+wheel or
  Ctrl+Alt+= / - for the interface size, PgUp / PgDn to scroll a panel.

**Languages**
- The interface comes in **English and Português (Brasil)**. **Preferences →
  Language** picks one: Auto (follows the system language, English when
  Haumea has no catalog for it), English, or Português (Brasil); the change
  applies at once. Windows, menus, tooltips, messages and Map Check are
  translated; the Pawn Haumea writes, native and model names, file names and
  what Namaka reads stay English. The Portuguese
  [README](README.pt-BR.md) and [user guide](docs/USER_GUIDE.pt-BR.md) go with it.
- Adding a language takes no build: a UTF-8 `lang/<code>.po` file beside the
  editor shows up in the list. `tools/i18n/extract.py` makes the template.
  See [CONTRIBUTING.md](CONTRIBUTING.md).
- A saved window layout survives the change: windows keep their place and
  dock.

**Show Pressed Keys**
- For tutorials and recordings, off by default: **Windows → Show Pressed
  Keys**, or **Preferences → General**. Over the 3D view (bottom left, or the
  corner you pick) it shows the keys, mouse buttons and wheel the editor got
  as key caps: "Ctrl + D", "RMB + W" while flying, "RMB + drag". A shortcut
  that ran adds its name ("Ctrl + D: Duplicate selection"). While a text
  field has the keyboard only Ctrl and Alt combinations show, without a name,
  so typed text never ends up in a recording.
- **Show what Namaka is doing** (on by default, and shown even with the keys
  off) adds a line for each thing the assistant does: "Namaka: placing
  elements: 2", "moving the camera", "looking at the scene", saving, Map Check
  and Test in game. Commands that only read are shown at most every two
  seconds, and a command that failed shows nothing.

**Updates**
- Haumea updates itself on Windows and Linux: a small banner says when a new
  version is out (What's new, Update now, Later, Skip this version), the
  download is checked against the release's SHA256SUMS, and it installs the
  next time Haumea starts, never while you work. Only Haumea's own files are
  replaced: your maps, settings and anything else in its folder (the GTA
  folder too) stay as they are, and a Haumea file you edited is kept as
  `<file>.local`. **Help → Roll back** returns to the previous version.
  **Preferences → Updates** turns checking or automatic downloads on and off
  and picks the channel. On macOS, or a package-manager install, the banner
  links to the release instead.

**Namaka** (formerly Agent Alpha), the AI assistant companion, installs from
**Windows → Namaka**: offline with Python 3.14, or downloading only a few
parts on 3.10-3.13. It has Repair, Update, Uninstall, a log, and copies the
MCP setup for Claude Code, Codex, Claude Desktop or any other MCP client
(JSON), whichever your assistant is. Its install sits in the config folder, so editor updates don't move
it. Once it is installed, Haumea listens for it on
every start (Preferences > Namaka > "Let Namaka connect" turns that off), and
a second Haumea never takes over the first one's connection.

**The agent** (Namaka) can edit the open map and every element kind,
run Map Check and Test in game, and grab frames with the panels.

### Also

- **A daily usage ping, only if you say yes**: the start screen asks once, and
  **Preferences → Privacy** changes it. It sends a random install ID, the
  version, channel and OS, at most once a day, to telemetry.itsneufox.xyz; off
  deletes the ID. The old Windows-only ping, which was on by default and went
  to another server, is gone, and its ID is deleted. See
  [Privacy](docs/USER_GUIDE.md#privacy).
- **Crash and error reports, only as you choose**: after a crash or an error,
  the next start asks before sending a report (View report, Send, Don't send,
  Always send), and **Preferences → Privacy** can make it Always or Never.
  Reports leave out your folders, user name and map names. Releases come with
  haumea-symbols-<version>.zip, so the developer can read their stacks.
- **Community files**: bug report and feature request forms, a pull request
  template, a contributing guide (including how to mark text for
  translation), a security policy and a code of conduct.
- **Help → About** says "(Dev build)" or "(PR build)" for test builds and
  ", Beta channel" for betas; releases say only the version.
- **Labels, sounds, map icons, gang zones and path points are editor icons**,
  not GTA models: a coloured T, speaker, pin, square or node that keeps its
  size on screen, shows faintly through walls, and is clicked, box-selected,
  moved and dropped to the ground like before. Placing one puts it on the
  surface. Pickups, actors, classes, NPCs and vehicles keep their models.
- **W/A/S/D fly the camera only while the right mouse button is held**, as in
  Unity and Unreal: hold it to look around and fly, with Shift (fast) and Alt
  (slow). On their own the keys are shortcuts, so `W`, `E` and `D` never move
  the view. It works with the mouse on the gizmo too, and a hold spent flying
  doesn't open the right-click menu. (The button is the look button in
  **Preferences → Camera**.) While it's held the cursor hides and stays put,
  so a turn never stops at the screen's edge, and it's back where it was on
  release.
- **See your whole map**: the **View** window has **World draw distance** (50
  to 3000 m) and **Map draw distance** (your map's own objects, 2000 m by
  default, whatever their model's draw distance, and never less far than the
  world), with **Fog** Game, Light (the default) or Off. The game's fog starts
  10 m out on a clear midday, which made everything past a few metres look
  faded. **Focus my map** dims the game's buildings. Player view keeps the
  game's distances. Remembered between sessions.
- **Material colours look as in game**: an object retextured with a colour
  (`SetObjectMaterial`, `SetDynamicObjectMaterial`) is tinted by it and lit as
  the SA-MP client lights it, instead of keeping its prelighting (a green
  tint came out orange) or turning black.
- **Ultrawide screens** (21:9, 32:9) show the world and the sky as wide as
  the screen, without stretching or gaps in the sky.
- **Version numbers as open.mp has them**: 1.0.0 plus a build number, e.g.
  1.0.0.57 (the commit count), with the commit next to it in About, the title
  bar and `haumea --version`. Releases come in two channels, production and
  beta (tags `v1.0.0.57-production`, `v1.0.0.57-beta`), and the updater
  compares all four numbers.
- **Haumea Studio runs from its own folder.** The first start asks for the
  **game folder** (GTA San Andreas, only ever read) and an optional
  **workspace** (the server you map for). It never writes into the game
  folder. Settings live in `~/.config/haumea` (`%APPDATA%\Haumea` on Windows,
  `~/Library/Application Support/Haumea` on macOS).
- **Start screen** on every launch: recent workspaces (pin, remove, open the
  folder), the selected workspace's recent maps with thumbnails, a new
  workspace or any folder as one, and the game folder with the SA-MP client
  version it has. "Open the last workspace automatically" skips it; File >
  Switch Workspace shows it again.
- **What players see**: the editor shows the stock game, SA-MP's models and
  the server's custom models, not your client mods.
- **Shortcuts**: windows open with Ctrl+number and F-keys, so the letters are
  free for editing. F1 lists them all, and Preferences → Shortcuts changes any
  of them.
- Generated Pawn compiles without warnings on open.mp and SA-MP 0.3.7,
  keeps variable names stable across saves, and is written with the same
  line endings (LF) on every platform.
- Saving writes a temporary file and renames it over the map, so a crash
  mid-save never leaves a cut-off `.pwn`.
- Undo covers every edit, including vehicles and the Nodes tool, and a drag
  or typing in a field is one step.
- Big maps are fast: a 20000-object map opens in about 0.1 s.
- **Accents and symbols show correctly in game.** open.mp and SA-MP clients
  draw text as Windows-1252, as pawno and Texture Studio write it, so maps are
  saved that way: "Ação", "Coração", "€" and "ñ" in labels, material text,
  textdraws and notes look the same in game as in the editor. Each map
  remembers its text encoding (a `// Text encoding:` line in the file), and
  Map Check names any character Windows-1252 can't hold (it's saved as `?`).
- Windows: paths with any characters (João, Жора) work on Windows 10 1903
  and later.

### What you need

- **Linux**: x86_64 with glibc 2.38 or newer (Ubuntu 24.04, Debian 13,
  Fedora 39 or newer), `libglfw3`, and `zenity` or `kdialog` for file
  dialogs. Test in game also needs the 32-bit C library for open.mp's server.
- **Windows**: Windows 10 or 11; 1903 or later for non-ASCII paths.
- **NPC features** (NPCs, paths, nodes, recordings) need an **open.mp**
  server with its NPCs component; SA-MP 0.3.7 has no NPC natives.
- **Custom models** need SA-MP 0.3.DL or open.mp with `artwork.enable` on,
  the models in the server's `models/artconfig.txt`, and players on 0.3.DL or
  an open.mp client.
- **Checkpoints** and Streamer saves need the streamer plugin.
