# silicon

A tiny pixel-art operating system for x86_64, written from scratch in C++ and NASM assembly
and made to run in QEMU. It has its own bootloader, firmware screens, kernel, filesystem,
network stack, web browser and desktop. Nothing is borrowed from GRUB, a libc, a TCP/IP stack,
a TLS library or a GUI toolkit.

silicon boots into a full-screen terminal. Type `ui` to start the desktop, or set
**Boot Into: Desktop** in the firmware setup.

## Build and run

You need Python 3 with Pillow, NASM, a MinGW-w64 `g++`/`ld`/`objcopy`, and QEMU on your `PATH`.

```bash
python build.py run
```

- `python build.py`: builds `build/silicon.img`, a 64 MiB raw disk image
- `python build.py run`: builds and boots it in QEMU
- `python build.py --fresh-disk`: wipes your files and starts from the starter files
- `python build.py clean`: deletes `build/`

Files you create are stored in the SiliconFS region of `build/silicon.img`. Rebuilding keeps
them unless you pass `--fresh-disk`.

QEMU runs with hardware acceleration (WHPX on Windows) when it's available and falls back to
TCG. Set `SILICON_ACCEL=tcg` to force software emulation. With WHPX the window uses QEMU's SDL
display: its GTK window stalls a WHPX guest. The VM gets an RTL8139 network card
on QEMU's user-mode network, so the internet works with no setup.

## Booting

1. **Firmware POST**: a BIOS-style screen counts the memory and lists the CPU, the disk, the PCI
   devices and the network card.
   - **DEL** or **F2**: the setup utility (Main / Advanced / Boot / Exit). Use the arrows to move,
     Enter or `+`/`-` to change a value, and **F10** to save.
     - Advanced: the network card, default shell, terminal font and theme, sounds, FPS counter and clock format.
     - Boot: terminal or desktop, and fast boot.
   - **F12**: a boot menu (terminal, desktop, setup or reboot).
   - Type `setup` in the full-screen terminal to open the setup utility without rebooting.
2. **Splash**: the chip warms up while the network comes up (DHCP).
3. **Terminal** (or straight to the desktop).

## What's inside

| part | where | notes |
|---|---|---|
| stage 1 | `src/boot/stage1.asm` | 512-byte MBR that loads stage 2 with INT 13h extensions |
| stage 2 | `src/boot/stage2.asm` | A20, E820 memory map, unreal mode to load the kernel at 1 MiB, VBE 1024x768x32, page tables, long mode |
| firmware | `src/kernel/firmware.cpp` | POST screen, setup utility, boot menu |
| kernel | `src/kernel/` | GDT/IDT/PIC, a TSC clock calibrated against the PIT, heap, PS/2 keyboard, mouse (vmmouse or PS/2), RTC, ATA with bus-master DMA, PCI, power, a non-blocking PC speaker |
| SiliconFS | `src/kernel/fs/` | in-memory tree saved to its own disk region (checksummed, only changed sectors are written) and loaded at boot; has a trash |
| network | `src/kernel/net/`, `drv/rtl8139.cpp` | RTL8139 driver, Ethernet, ARP, IPv4, ICMP, UDP, DHCP, DNS, TCP (retransmission, flow control), HTTP/1.1 (chunked, redirects), TLS 1.2 and 1.3 (X25519, AES-128-GCM, SHA-256) |
| terminal | `src/kernel/term/` | pixel console with scrollback and ANSI colours; bash, zsh and fish flavoured shells |
| desktop | `src/kernel/ui/` | compositor with damage tracking and per-window surfaces, window manager with animations, snapping and overview, top bar, the silicon-die dock, launcher, alt-tab, notifications, screensaver |
| web | `src/kernel/ui/html.cpp`, `css.cpp`, `apps/browser.cpp` | HTML tokenizer and flow layout; a CSS engine (selectors, cascade, `!important`, `@media`, custom properties) for display, colours, backgrounds, borders, fonts, margins, padding and centred columns; the Beam browser |
| images | `src/kernel/gfx/imgdec.cpp` | PNG (with its own inflate), GIF, JPEG (baseline and progressive) and BMP decoders |
| fonts | `src/kernel/gfx/vfont.cpp`, `art/fonts/` | scalable fonts: the PDF engine's TrueType parser and rasterizer draw DejaVu Sans (regular and bold, embedded in the kernel) at any size, with a glyph cache |
| keyboard | `src/kernel/drv/kbd.cpp` | layouts (US, UK, German, French, Spanish, Dvorak) with AltGr and dead keys; every key press carries a Unicode code point |
| modules and SDK | `sdk/`, `src/kernel/ui/apps/module.cpp` | third-party apps (`.sapp`) built against a stable function table (`sdk/silicon.h`) and loaded by the kernel |
| firewall | `src/kernel/net/fw.cpp` | a stateless packet filter on the IP layer, and the HTTP proxy setting |
| pdf | `src/kernel/pdf/` | a PDF reader: object parser (xref tables and streams, object streams, repair of broken files), filters (Flate, LZW, ASCII85, ASCIIHex, RunLength, predictors), an anti-aliased rasterizer with stroking, dashes and clipping, colour spaces and functions, shadings, images, TrueType / CFF / Type 1 font parsers with a bitmap-font fallback, annotations and text extraction. `tools/pdftest/` is a host harness that renders a PDF to a PPM |
| art | `tools/make_art.py`, `tools/assets.py` | generates silicon's own icons and wallpapers; every PNG is packed into a compressed `.sil` blob linked into the kernel |
| font | `tools/font.txt` | silicon's own 8x12 pixel font, hand-drawn as ASCII art |

**Security note:** HTTPS traffic is encrypted, but server certificates are **not verified**
(silicon has no CA store yet). Don't type anything secret into a web page.

## The desktop

- **Top bar**: click the chip for the system menu (About, Settings, Launcher, Overview, Monitor,
  Screensaver, Terminal mode, Restart, Shut down). The focused app's name and menus sit next to it.
  On the right: your user name, an optional FPS counter, a memory meter, network signal, search
  and the clock.
- **The dock** is a chip. Apps sit in cells on a silicon die, and bond wires run from the die out to
  gold pins on every side.
  - Hovering lifts the icon, lights the app's copper trace and fires a pulse along the bus to the pins.
  - A running app turns on its LED and a pin, and opening an app makes its icon bounce.
  - When many apps are open, the icons shrink so the die still fits.
- **Windows** open, close and minimize with animations.
  - Move them by the title bar and resize from the bottom-right corner.
  - Double-click a title bar (or press F11) to maximize.
  - Drag a window to the left or right edge to snap it to half the screen, or to the top to maximize.
- **Overview** (F3) shows every window at once. **Alt+tab** shows live thumbnails.
- **Scrolling**: the mouse wheel scrolls smoothly everywhere, and scrollbars can be dragged.
- **Clipboard**: copies inside silicon also go to the host clipboard and host copies come back in. It carries text (including emoji), pictures (Photos and Capture copy them, Photos and Files paste them) and lists of files (Files copy/cut/paste; the paths are also on the clipboard as text).
- **Drag and drop**: drag desktop icons anywhere (they snap to a grid and keep their place in `~/.config/desktop`), or drag files and folders from the desktop or Files onto another window: Terminal pastes the path, Slate, Write, Photos, PDF, Docs, Hex, Beam and Archive open it, Wallpapers uses a picture, Calendar imports an `.ics`, Files moves it, and the Trash or the dock's trash can deletes it.
- **Notifications**: every notification is kept in the notification centre (the bell in the top bar). Do not disturb silences pop-ups and sounds, and any app can be muted from the list.
- **Desktop icons** show what's in `/home`. Double-click to open; right-click for a menu.
- **Screensaver**: after 5 idle minutes (or from the system menu), a warp through stars and chips.

| shortcut | does |
|---|---|
| `ctrl+space` | launcher: search apps, files, web addresses, or the web |
| `alt+tab` | switch windows |
| `f3` | overview |
| `f11` | maximize |
| `ctrl+alt+t` | new terminal |
| `ctrl+w` / `alt+f4` | close window |

### Apps

- **Beam**, the web browser.
  - Tabs (`ctrl+t` new, `ctrl+w` close, `ctrl+tab` switch, middle-click a tab to close it).
  - Navigation: links, back/forward, bookmarks and history.
  - Find in page (`ctrl+f`, Enter / shift+Enter to step), text selection by dragging or double-clicking, `ctrl+c` copy, `ctrl+a` select all, and a right-click menu (open link, open in new tab, copy link address, copy, back, reload, find, source).
  - Pages: CSS (from `<style>`, linked style sheets and `style=""`), forms (GET and POST, with paste), pictures, large emoji in headings, view source and save page.
  - A PDF link is downloaded to `/home/downloads` and opens in the PDF viewer.
  - View > Page styles turns CSS off.
  - Type words in the address bar to search with DuckDuckGo Lite.
  - Sites that work well: text.npr.org, lite.cnn.com, Hacker News, Wikipedia, xkcd, wiby.me and 68k.news.
- **PDF**: continuous scrolling, zoom and fit-to-width, an outline sidebar, text selection (drag, double-click, `ctrl+a`) with `ctrl+c`, and find (`ctrl+f`). Open a `.pdf` from Files, Beam or the shell (`open file.pdf`). Not supported yet: encrypted files, CCITT / JBIG2 / JPX images and mesh shadings.
- **Photos**: a gallery of every picture in SiliconFS and a viewer (fit, zoom, pan, next/previous).
- **Terminal**: select with the mouse (double-click a word, triple-click a line), `ctrl+shift+c` (or `ctrl+c` while something is selected) copies, `ctrl+v` pastes, right-click for a menu, and emoji show up as pictures.
- **Files**: multi-select with ctrl-click, shift-click, a rubber band or `ctrl+a`; copy, cut and paste files with `ctrl+c` / `ctrl+x` / `ctrl+v`; drag several items at once.
- **Apps** lists every app, utility and game in one searchable grid (it's in the dock).
- **Write**: a Unicode text editor with scalable fonts, sizes from 10 to 72, bold, right-to-left text (Hebrew, and Arabic with letter shaping and bidirectional ordering), UTF-8 files and a keyboard menu for the layouts. Not supported: ligatures other than Arabic lam-alef, CJK (the font has no glyphs for it) and complex scripts such as Devanagari.
- **Docs**: reads Word (`.docx`), OpenDocument (`.odt`, `.ods`) and Excel (`.xlsx`) files and CSV/TSV as pages and grids, read-only, with "save as text / csv".
- **Mail**: POP3 receive and SMTP send (plain or implicit TLS), RFC 2047 headers, quoted-printable and base64 bodies, one account. TLS certificates are not verified, and the password is stored unencrypted in `~/.config/mail`.
- **Firewall**: packet filter rules (allow or deny, inbound or outbound, protocol, port, address), a log of what was blocked, and an HTTP proxy setting used by Beam, wget and curl.
- **Slate** (text editor), **Pixel** (pixel painter), **Calculator**, **Monitor**, **Settings**, **Wallpapers**,
  **Clock**, **Piano** (two octaves on the PC speaker, with recording and a demo tune), **Emoji** (a picker
  that copies to the clipboard), **Trash** and **About**.
- **Utilities**:
  - **Calendar**: month view with events saved in `~/.config/calendar`; imports `.ics` files, subscribes to an `.ics` address (Sync now) and exports everything.
  - **Tasks**: a to-do list.
  - **Hex**: a hex viewer for any file.
  - **Storage**: what fills the disk, by folder and the largest files.
  - **Convert**: unit converter (length, mass, temperature, data, time, speed, area).
  - **Palette**: an HSV colour picker with saved swatches.
  - **Capture**: screenshots with a delay, saved as BMP in `/home/pictures`.
  - **Weather**: a forecast from wttr.in.
  - **Typing**: a typing speed test.
  - **Network**: interface details, connections and ping.
  - **Archive**: opens and extracts `.zip` files (stored and deflate) and creates new ones.
- **Games** (open them from the Games library; high scores are saved in `/home/games`):
  - Arcade: Stacker (falling blocks with hold and a ghost piece), 2048, Breakout (power-ups and five level layouts), Flappy Chip, Snake, Pong, Invaders, Asteroids and Runner.
  - Boards and cards: Minesweeper, Reversi, Connect Four, Memory, Sudoku, Solitaire (Klondike) and Chess (you play white against a small alpha-beta engine; castling, en passant and promotion to a queen are supported).
  - Words: Wordy.

## Writing apps for silicon

`sdk/` has everything needed to build an app module: `silicon.h` (the function table: drawing, files, clipboard, notifications, timing, memory), `build_app.py`, and an example in `sdk/examples/hello`.

```bash
python sdk/build_app.py sdk/examples/hello/main.cpp -o hello.sapp
```

Copy the `.sapp` into silicon (`wget` it, or put it on the disk image) and open it from Files or with `open hello.sapp`. The example is also built into the kernel and installed as `/home/apps/hello.sapp`. Modules run in the kernel's address space and can crash the system or read anything, so only run modules you trust; real processes and memory protection are still to do.

## Shells

The terminal speaks three dialects. Switch with `bash`, `zsh` or `fish` (and `exit` to go back), or
make one the default with `chsh -s zsh`. All three support:

- pipes `|`, command lists `;` `&&` `||`, and redirection `>` `>>` `<`
- variables (`NAME=value`, `export`, `$VAR`, `${VAR}`, `$?`), `~`, and `*` / `?` globbing
- aliases, history with `!!` `!$` `!prefix`, ctrl+r reverse search, tab completion
- start-up files: `~/.bashrc`, `~/.zshrc`, `~/.config/fish/config.fish`

Each one has its own prompt. zsh and fish also highlight syntax and suggest from your history as
you type.

## Shell commands

Type `help` for the full list. Some highlights:

```
ls -l   tree   cat   edit notes/todo.txt   echo hi > file.txt   grep todo notes/todo.txt | sort | uniq
fetch   cal   mem   df   theme amber   zoom   wallpaper 2   chipsay hello   snake   rain   life
kbd de   (keyboard layout)
ifconfig   ping example.com   nslookup wikipedia.org   curl -I https://example.com
wget -O /home/pictures/cat.jpg https://...   netstat   lspci   setup   ui   reboot   shutdown
```

## Third-party fonts

DejaVu Sans and DejaVu Sans Bold (`art/fonts`) are free fonts derived from Bitstream Vera, distributed under the Bitstream Vera / DejaVu licence, which allows embedding and redistribution with the licence notice (https://dejavu-fonts.github.io/License.html).

## Art credits

The icons in `art/icons` and the wallpapers in `art/wallpapers` come from the AetherOS project.
The icons and wallpapers in `art/silicon` (chip, calculator, pixel, clock, launcher, browser,
photos, piano, pdf, emoji, the utility and game icons, wafer, circuit dusk, silicon valley and others) are generated by
`tools/make_art.py` and `tools/make_icons2.py`.

## Testing the PDF engine on the host

`sh tools/pdftest/build.sh` compiles the PDF code with the host `g++` into `tools/pdftest/pdftest.exe`:

```bash
tools/pdftest/pdftest.exe file.pdf info             # pages, title, outline
tools/pdftest/pdftest.exe file.pdf 1 1.5 out.ppm    # render page 1 at 1.5x
tools/pdftest/pdftest.exe file.pdf text 1           # extracted text of page 1
```

## Testing headless

`tools/qemu_ctl.py` boots QEMU without a window and drives it through the monitor:

```bash
python tools/qemu_ctl.py start wait 4 type "ui\n" wait 2 shot build/shots/desktop.png quit
```

From Git Bash, run `export MSYS_NO_PATHCONV=1` first, so URLs reach QEMU unchanged.

## Modern look, processes, media, safer storage

- **Look:** flat, anti-aliased UI: rounded windows with soft shadows, proportional DejaVu text (DejaVu Sans Mono in the terminal), smooth icons/cursors/wallpapers generated by `tools/make_modern.py`.
- **Programs in ring 3:** `sdk/build_uapp.py` builds `.uapp` programs that get their own address space, threads, a preemptive scheduler and a system-call API (`sdk/user/silicon_user.h`). A crash (bad pointer, privileged instruction) ends only that process. Try `run /home/apps/guard.uapp`, `ps`, `kill`, and Monitor > Programs. Older `.sapp` modules still run inside the kernel without protection.
- **Media:** AC97 sound plus the Media app: WAV (PCM/IMA ADPCM), MP3 (minimp3, CC0), FLAC, AVI with Motion JPEG.
- **Storage:** commits alternate between two disk slots with checksums (crash-safe, not a classic journal); Unix-style owners and permissions (`ls -l`, `chmod`, `chown`, `sudo`, `id`); `/system` is root-owned.
- **Not done:** Wi-Fi, Bluetooth, IPv6, VPN, removable/USB drives and a FAT driver.
