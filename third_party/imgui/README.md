# Dear ImGui

The window program's own screens (the launcher, the settings and the game
builder) are drawn with Dear ImGui (https://github.com/ocornut/imgui),
release `v1.92.9b`, commit `f1cc2ae15e53a861a874c3034aae6798fde194ab`
(31 July 2026). It is licensed under the MIT licence; see `LICENSE.txt` in
this directory, which is shipped with the program.

Copied unmodified: the library's own sources and headers, and the two
backends for SDL 3 (`backends/imgui_impl_sdl3.*`, its window and input, and
`backends/imgui_impl_sdlrenderer3.*`, its drawing with SDL's renderer).
Nothing else of the repository (examples, demo, documents, fonts) is here.

To update: replace the files with those of a newer release, record its tag
and commit here, build, and look at the screens.
