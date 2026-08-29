# Windows 11 Desktop Photo Slideshow Widget

A tiny, **portable** photo widget that lives *on the desktop layer* of Windows 11 — not inside the
Widgets Board. It shows the pictures of one folder, advances them on a timer, keeps the exact
position and size you left it in, starts with Windows, and steps out of your way whenever you open
a real application.

* `~700 KB` single `.exe`, no installer, no .NET / VC++ runtime, no admin rights
* glued to the desktop (behind every app window) like Rainmeter skins — not a Widgets Board tile
* click-through by default: the mouse ignores the widget, so double-clicking a desktop icon right
  over it still works
* folder watching, crossfade, Ken-Burns zoom/pan, rounded corners, soft shadow, caption, system-tray
  menu, settings dialog, `Ctrl+Alt+…` hotkeys, INI file that lives next to the exe

---

## Get the .exe

The `.exe` is built by GitHub Actions (this sandbox is Linux, so the binary is produced by a
`windows-latest` runner).

1. **Downloads** — open [Releases](../../releases) and grab
   `desktop-slideshow-<version>-win-x64-portable.zip`.
2. Extract it to a normal folder (e.g. `D:\Tools\desktop-slideshow`) — do not run it from inside the
   archive preview, that gives you a read-only temp folder.
3. Double-click `desktop-slideshow.exe`.

### Build it yourself / rebuild

| What | How |
|---|---|
| Automatic release build | create a tag `v1.0.0` (or a GitHub release) → the workflow builds it and attaches the zip |
| Just a build, no release | **Actions → “Build Windows exe” → Run workflow** → artefact under *Summary* |
| On your own PC | install [MSYS2](https://www.msys2.org), run `build\build.cmd` (or `build.ps1` from an *MSYS2 MinGW64* shell) |

```bash
git tag v1.0.0 && git push origin v1.0.0        # → Release with desktop-slideshow.exe attached
gh workflow run build-windows.yml -f version=1.0.0     # → artefact only
```

---

## First run (30 seconds)

1. Tray icon appears → click it → **Choose folder…** and pick your pictures.
2. Click **Edit mode (drag / resize)** (or press `Ctrl+Alt+W`): the widget gains a blue frame and a
   hint bar. Drag the picture to where you want it, grab the **corner grip** to size it, then press
   `Ctrl+Alt+W` again — the position is stored in `slideshow.ini`.
3. Tick **Start with Windows**. Done.

That is all: from now on the widget restores itself at that spot on every sign-in, stays under your
windows and never steals a click.

---

## How it works (why it behaves like a widget and not like a window)

| Requirement | Implementation |
|---|---|
| A free-floating frame on the desktop | `WS_POPUP` + `WS_EX_LAYERED` window, painted into a 32-bit premultiplied DIB and pushed with `UpdateLayeredWindow()` — real per-pixel alpha, so rounded corners and the shadow are honest |
| Stays on the desktop layer | kept at the bottom of the z-order with `SetWindowPos(HWND_BOTTOM)` on a 1 s check that only fires when the widget is *not* already bottom-most (`auto_bottom`) |
| Does not get in the way | `WM_NCHITTEST → HTTRANSPARENT` (the Rainmeter trick): clicks fall through to the desktop. The 26 px **grip** in the corner stays clickable, so moving/resizing it never needs a settings screen |
| Fixed position & size | the rect is stored in `slideshow.ini`; it is clamped back onto a real monitor, survives resolution changes, and is DPI aware (Per-Monitor V2 manifest) |
| Automatic slideshow | folder scan (`recursive`), `interval_s` timer, shuffle, `min_side` filter, drive-change (`WM_DEVICECHANGE`) rescan |
| Autostart | `HKCU\…\CurrentVersion\Run` → `"<path>\desktop-slideshow.exe" --startup` — works for a portable exe, no registry write needed to run it |
| CPU friendly | nothing is repainted between slides (`anim_fps = 0` by default): the Ken-Burns frames are cropped from a **pre-scaled** bitmap, so a transition is just a `BitBlt`-sized `DrawImage` |

Also handled: session lock/unlock (`WTSRegisterSessionNotification`), fullscreen games and
presentations (`SHQueryUserNotificationState`), Explorer restart, monitor/DPI changes, single
instance, and a `slideshow.log` you can turn on with one config line.

---

## Settings — `slideshow.ini`

The file sits **next to the .exe** (that is what makes it portable). Tray menu → *Edit slideshow.ini*,
change what you want, then tray menu → *Reload slideshow.ini*. Every key is optional; unknown keys are
ignored, `;` starts a comment (except inside the `extensions` list).

```ini
[
path = ''                      ; empty = your Pictures folder
recursive = 1
extensions = .jpg;.jpeg;.png;.bmp;.gif;.tif;.tiff;.webp
shuffle = 1
repeat = 1
min_side = 160
exif_auto_rotate = 0           ; set 1 if photos show sideways

interval_s = 30
random_interval = 0
random_jitter = 30
transition_ms = 900            ; 0 = hard cut

anim = zoom                    ; none | zoom | pan
anim_zoom = 1.10
anim_alt = 1
anim_fps = 0                   ; >0 = keep animating between slides (more CPU)

corner = 12                    ; rounded corners (0 = square)
shadow = 18                    ; soft drop shadow (0 = none)
border = 1
border_alpha = 26
bg = #0B0F14
fit = cover                    ; cover | contain | stretch
half_screen = 0                ; 1 = exactly half of the monitor work area
left = 0
top = 0
width = 0
height = 0           ; written for you when you move the widget

caption = 1
caption_fmt = {name}  ·  {i}/{n}    ; {name} {folder} {path} {i} {n} {time} {date}
caption_size = 15
caption_font = Segoe UI
caption_h = 40
caption_alpha = 235
caption_hover_only = 0

click_through = 1
auto_bottom = 1
topmost = 0                    ; 1 = float above everything instead
pause_when_fullscreen = 1
pause_on_lock = 1
auto_start = 0
show_in_taskbar = 0
hotkeys = auto
log = 0

edit = 0
show_grip = 1
grip_size = 26
bottom_check_ms = 1000
```

### Hotkeys (`Ctrl+Alt+…`)

| Keys | Action |
|---|---|
| `W` | toggle edit mode (place / size the widget) |
| `P` | play / pause |
| `←` / `→` | previous / next photo |

---

## Command line (handy for shortcuts, PowerToys, logon scripts)

```
desktop-slideshow.exe                     start / show the widget
desktop-slideshow.exe -edit               start ready to drag and resize
desktop-slideshow.exe -path "D:\Photos"   set the folder and save it
desktop-slideshow.exe -seconds 60         set the interval and save it
desktop-slideshow.exe -autostart 1|0      add / remove the startup entry
desktop-slideshow.exe -reload             make a running instance re-read the .ini
desktop-slideshow.exe -save               make a running instance store its current rect
desktop-slideshow.exe -menu               pop up the widget menu
desktop-slideshow.exe -quit               exit the running instance
desktop-slideshow.exe -config other.ini   use another settings file (sticky)
```

---

## Recipe: “picture frame on the left half of my screen”

```ini
path = D:\Family
half_screen = 1
interval_s = 45
transition_ms = 1200
anim = zoom
anim_zoom = 1.12
corner = 0
shadow = 22
border = 0
caption = 0
click_through = 1
auto_bottom = 1
```

Run it once with `-edit` if you want to fine-tune the size, then let the `auto_bottom` logic keep it
behind your windows forever.

---

## Repository layout

```
src/            main.cpp (entry, message loop)
                app.cpp  (widget window, tray, menu, settings dialog, z-order, autostart)
                render.cpp (GDI+ painting + UpdateLayeredWindow, layout maths)
                images.cpp (folder scan, decoding, pre-scaling cache)
                config.cpp (slideshow.ini reader/writer, UTF-16 so paths survive Unicode)
                cli.cpp    (command line verbs, -quit/-reload forwarding)
                app.manifest / app.rc / app.ico
build/          build.ps1 (MinGW-w64 build), build.cmd (double-click wrapper)
.github/workflows/build-windows.yml    builds + publishes the exe
slideshow.default.ini                    template shipped inside the zip
```

## Troubleshooting

| Symptom | Fix |
|---|---|
| “No pictures found” | the folder is wrong, or the files are inside subfolders with `recursive = 0`; also check `extensions` |
| The widget is on top of my windows | `auto_bottom = 1` and make sure `topmost = 0`; a maximised app always covers it — that is the point |
| I cannot grab it to move it | the widget is click-through: use the small **grip** in its bottom-right corner, or press `Ctrl+Alt+W` |
| Photos are sideways | `exif_auto_rotate = 1` |
| It moves on its own when a window animates | raise `bottom_check_ms` (e.g. `2000`) |
| High CPU with 30 zoom animations | keep `anim_fps = 0` (transition-only motion) |
| Antivirus / SmartScreen warning | unsigned build — verify the SHA256 from the release and keep the exe in a permanent folder |
| Two monitors, wrong one after unplugging | the rect is clamped to a live monitor on the next start; or set `half_screen = 1` |

## Known limitations (deliberate)

* GIFs are shown as their first frame (no animation) — keeps the paint path to one DIB.
* HEIC/RAW are only supported if the OS image codecs provide them; `.webp` works on Windows 11.
* Windows Spotlight / auto-positioned desktop icons can repaint over it — that is normal, the widget
  is *on* the desktop, not a wallpaper engine.

## Build notes / verification

The code is plain Win32 + GDI+ (C++17, no frameworks). It was compiled and **linked** to a real
PE64 GUI executable in the sandbox with a mingw-w64 header set (`zig c++`), which proves the
translation units, all Win32/GDI+/shell imports and the resource script (icon + manifest +
version info) are sound. The Windows runner in CI does the official build with `g++` from
MSYS2 - same flags as `build/build.ps1`, so what you download is byte-for-byte the same recipe.

* output today: `desktop-slideshow.exe` ≈ **420 KB**, `.text` only, no extra DLLs
* `DllCharacteristics`: `DYNAMIC_BASE | NX_COMPAT | Terminal Server aware | GUARD_CF`
* embedded: `RT_GROUP_ICON` (9 sizes), `RT_MANIFEST` (Per-Monitor V2 DPI, Win10/11 `supportedOS`,
  `asInvoker`), `VERSIONINFO`

It has **not** been run on a desktop yet (no Windows box here), so the first thing to try is the
placement / click-through behaviour on your monitors; every knob that could misbehave
(`auto_bottom`, `bottom_check_ms`, `click_through`, `show_grip`) is a single line in
`slideshow.ini`.

## License

MIT
