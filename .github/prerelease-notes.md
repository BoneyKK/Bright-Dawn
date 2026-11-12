Automated pre-release, built from `main` after all checks passed.

**This does not run the game.** The project is at an early stage; this package
contains `bdtool`, a command-line tool that validates and inspects a ROM you
supply yourself. No ROM and no game data are included.

- `bright-dawn-windows-x86_64.zip`: Windows, 64-bit
- `bright-dawn-linux-x86_64.tar.gz`: Linux, 64-bit (statically linked)
- `SHA256SUMS.txt`: checksums of the packages

Unpack, then run `bdtool --help`. See the README for what works today.

`bdtool --version` prints this release's tag, and `validate`, `info` and
`survey` print it as their first line. Please include that line when sharing
output.
