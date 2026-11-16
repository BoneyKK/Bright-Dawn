Automated pre-release, built from `main` after all checks passed.

**Early and not yet playable.** `bright-dawn` runs Golden Sun: Dark Dawn from
a ROM you supply yourself, in a window, with every instruction of the game
interpreted: about a third of the console's speed, without sound, and
untried beyond the first screens. No ROM and no game data are included.

- `bright-dawn-windows-x86_64.zip`: Windows, 64-bit
- `bright-dawn-linux-x86_64.tar.gz`: Linux, 64-bit (needs X11 or Wayland)
- `SHA256SUMS.txt`: checksums of the packages

Unpack and start `bright-dawn`. Its launcher asks for your ROM (and remembers
it), plays it, holds the settings, and can build the game from your ROM, its
code translated to C++ and compiled on your computer, from the `builder`
folder in the package. Building needs Python 3.10+, CMake 3.20+ and a C++
compiler (on Windows, Visual Studio or its Build Tools with "Desktop
development with C++"). The built game does not get far yet: interrupts are
not yet taken in translated code.

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
