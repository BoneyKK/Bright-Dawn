# Working on Bright Dawn with Claude Code

## Code map (graphify)

The repository's code and docs are mapped into a graph by
[graphify](https://github.com/Graphify-Labs/graphify), in `graphify-out/`
(not committed; `.graphifyignore` says what is left out). How to install it
and make the map: [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md#code-map-graphify).

- Before the first search or file read for a question about the code, ask
  the map: `graphify query "<question>"`. For one thing:
  `graphify explain "<name>"`. For how two things connect:
  `graphify path "<A>" "<B>"`. For a section of a doc:
  `graphify section "<heading>"`. Then read only the lines the map points to.
- Give the same rule to every subagent that explores the code.
- After a change to code or docs, run `graphify update .` (local, no model,
  about 20 seconds) so the map stays current.
- `graphify-out/GRAPH_REPORT.md` is for a look at the whole design, when a
  query is not enough.
- If `graphify` is not installed or `graphify-out/graph.json` is missing,
  work as usual and say so; do not stop for it.
- Never point graphify at a ROM, at anything made from one (extracted files,
  translated code), or at a folder outside the repository, and never use its
  `add` command or a model backend for it: the map must hold only the
  repository's own files.
