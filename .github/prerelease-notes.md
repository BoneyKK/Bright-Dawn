Automated pre-release, built from `main` after all checks passed.

**Early and not yet playable.** `bright-dawn` runs Golden Sun: Dark Dawn from
a ROM you supply yourself, in a window, with every instruction of the game
interpreted: about a third of the console's speed, without sound, and
untried beyond the first screens. No ROM and no game data are included.

- `bright-dawn-windows-x86_64.zip`: Windows, 64-bit
- `bright-dawn-linux-x86_64.tar.gz`: Linux, 64-bit (needs X11 or Wayland)
- `SHA256SUMS.txt`: checksums of the packages

Unpack, then start `bright-dawn` with the path of your ROM, for example by
dragging the ROM onto it on Windows, or `./bright-dawn your-rom.nds` on
Linux. `bright-dawn --help` lists the keys and options.

Keyboard: arrow keys, X = A, Z = B, S = X, A = Y, Q = L, W = R, Enter =
Start, Right Shift = Select. The left mouse button on the touch screen is the
stylus. Controllers work by button position, as on the DS. F11 or Alt+Enter:
full screen. Tab: screens side by side or stacked. F12: sharp whole-number
scaling or filling the window.

Nothing is saved between runs yet. `bright-dawn --version` prints this
release's tag; please include it when reporting a problem.
