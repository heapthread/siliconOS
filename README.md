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
  - Navigation: links, back/forward, bookmarks and history.
  - Pages: CSS (from `<style>`, linked style sheets and `style=""`), forms (GET and POST), pictures, view source and save page.
  - View > Page styles turns CSS off.
  - Type words in the address bar to search with DuckDuckGo Lite.
  - Sites that work well: text.npr.org, lite.cnn.com, Hacker News, Wikipedia, xkcd, wiby.me and 68k.news.
- **Photos**: a gallery of every picture in SiliconFS and a viewer (fit, zoom, pan, next/previous).
- **Terminal**, **Files**, **Slate** (text editor), **Pixel** (pixel painter), **Calculator**, **Monitor**,
  **Settings**, **Wallpapers**, **Clock**, **Piano** (two octaves on the PC speaker, with recording and
  a demo tune), **Trash** and **About**.
- **Games**: Stacker (falling blocks with hold and a ghost piece), 2048, Breakout (power-ups and five
  level layouts), Flappy Chip, Minesweeper, Snake and Reversi. High scores are saved in `/home/games`.

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
ifconfig   ping example.com   nslookup wikipedia.org   curl -I https://example.com
wget -O /home/pictures/cat.jpg https://...   netstat   lspci   setup   ui   reboot   shutdown
```

## Art credits

The icons in `art/icons` and the wallpapers in `art/wallpapers` come from the AetherOS project.
The icons and wallpapers in `art/silicon` (chip, calculator, pixel, clock, launcher, browser,
photos, piano, the game icons, wafer, circuit dusk, silicon valley and others) are generated by
`tools/make_art.py`.

## Testing headless

`tools/qemu_ctl.py` boots QEMU without a window and drives it through the monitor:

```bash
python tools/qemu_ctl.py start wait 4 type "ui\n" wait 2 shot build/shots/desktop.png quit
```

From Git Bash, run `export MSYS_NO_PATHCONV=1` first, so URLs reach QEMU unchanged.
