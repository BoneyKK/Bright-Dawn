# stb_vorbis

`stb_vorbis.c` is the Ogg Vorbis decoder of the stb libraries
(https://github.com/nothings/stb), version 1.25, copied unmodified from commit
`6b5799bae8a1fe3792691ef4b72ae26434c3e799`. Its SHA-256 is
`97a8febc738e6a29fafc8d8bc557c7f17ad3db7ba6544485f59395cc6008b764`. It is in
the public domain, or under the MIT licence, as you choose; see `LICENSE.txt`
in this directory, which is shipped with the program.

It decodes the replacement music (`src/audio/vorbis_file.cpp`, the only file
that includes it). It was chosen over libvorbis, which needs libogg and a build
of its own, because it is one file without dependencies, decodes as it reads,
and can seek to an exact sample, which loop points need. The files it reads
are the player's own music, not data from the internet. It is compiled as it
comes (`bd_stb_vorbis`), without this project's warning flags or sanitizers.

To update: replace the file with a newer release, record its version, commit
and SHA-256 here, and run the tests (`bd_tests`, the `vorbis_` tests).
