Automated pre-release, built from `main` after all checks passed.

**Early and not yet playable.** `bright-dawn` runs Golden Sun: Dark Dawn from
a ROM you supply yourself, in a window, with every instruction of the game
interpreted: about a third of the console's speed, without sound, and
untried beyond the first screens. No ROM and no game data are included.

- `bright-dawn-windows-x86_64.zip`: Windows, 64-bit
- `bright-dawn-linux-x86_64.tar.gz`: Linux, 64-bit (needs X11 or Wayland)
- `SHA256SUMS.txt`: checksums of the packages

Unpack and start `bright-dawn`. The first time (and after each update) it
opens the setup: choose your ROM (it is remembered) and build the game from
it, its code translated to C++ and compiled on your computer, which the
program then runs natively. Then the launcher opens: play, the settings, and
the log, over the book of the game's main menu, read from your ROM. Nothing needs installing for that: the compiler
(and on Windows, Python, if it is not installed) is downloaded for the build,
checked, and deleted with everything else the build made; about 2 GB of free
space is needed while it runs. The built game does not get far yet:
interrupts are not yet taken in translated code.

Keyboard: arrow keys, X = A, Z = B, S = X, A = Y, Q = L, W = R, Enter =
Start, Right Shift = Select. The left mouse button on the touch screen is the
stylus. Controllers work by button position, as on the DS. Escape (or a
controller's guide button): the menu, with the settings. F11 or Alt+Enter:
full screen. Tab: screens side by side or stacked. F12: whole-number scaling
or filling the window.

The game's save is not kept between runs yet. Every run writes a log, in
`logs` in the user data folder (`%LOCALAPPDATA%\BrightDawn` on Windows,
`~/.local/share/bright-dawn` on Linux; the launcher's Log page opens it):
please attach it, and the version `bright-dawn --version` prints, when
reporting a problem.
