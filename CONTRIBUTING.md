# Contributing to Patalania

Thanks for your interest! Patalania is a small volunteer project; bug reports, testing and patches are welcome.

## Reporting bugs

Use the issue tracker: <https://github.com/patalania/patalania/issues> (template "Bug report"). See [SUPPORT.md](SUPPORT.md) for what to include (there is no support, and no guarantee of a response). Do not post passwords or private data.

## Code changes

Patalania follows Firestorm release branches: after each Firestorm release, the new branch is merged into `main`. To keep these merges easy:

- Put new features in **new files** (`pat*.cpp/h`) where possible.
- Change Firestorm/Linden Lab files as little as possible. Mark every change:
  ```cpp
  // <PAT:Short-Name> reason
  ...
  // </PAT>
  ```
  and keep the original code in a comment when you modify existing lines, like Firestorm's `<FS:...>` tags.
- Keep internal identifiers (`FS...` setting names, `fs*.cpp` file names, `<FS:...>` comments) unchanged. Only visible texts, package names and online targets are rebranded.
- Never remove copyright notices or license headers.
- Do not add connections to new servers, telemetry or tracking. Do not remove the viewer statistics package required by Linden Lab's third-party viewer policy.
- Do not add proprietary libraries (FMOD, Kakadu, Havok) or assets.

## Pull requests

- One topic per pull request, small commits, commit messages starting with `PAT:`.
- Describe what changed, why, and how you tested it (grid, OS, with/without VR).
- No force-pushes on `main`.
- Contributions are accepted under the LGPL 2.1 (see [LICENSE](LICENSE)); you confirm that you have the right to submit your code under it.

## Building

See [doc/building_linux.md](doc/building_linux.md) and [doc/building_patalania.md](doc/building_patalania.md). There is no support for building.
