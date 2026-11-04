# Contributing

Development happens on GitLab, where the CI runs: lint, build and tests on
Windows and Linux, packaging and releases. The GitHub repository
[RayBotic-NDT/rbscan](https://github.com/RayBotic-NDT/rbscan) is a read-only
mirror of `main`; its tests do not run on GitHub. Issues and pull requests on
GitHub are welcome; they are ported to GitLab by a maintainer.

## Before sending a change

- Build and run the tests (see [Building from Source](README.md#building-from-source)):

  ```bash
  cmake --preset default
  cmake --build --preset release
  ctest --preset release
  ```

  On Linux, `ci/linux.sh build` and `ci/linux.sh test` do what the CI does.
- Format C++ with the repository's `.clang-format` (`clang-format -i <files>`);
  the CI rejects unformatted code.
- Keep the on-disk format compatible. A change to what is written to a scan
  goes into [docs/format.md](docs/format.md) or, for a schema,
  [docs/schemas.md](docs/schemas.md); a field that changes meaning or
  type needs a new schema version.
- After changing `schemas/*.json`, run `python tools/gen_schema_api.py`: it
  regenerates the typed API (`include/raybotic/scan/schemas.hpp`,
  `python/rbscan/schema_api.py`) and its round-trip test. The CI fails while
  they are out of date.
- Add a line to the `Unreleased` section of [CHANGELOG.md](CHANGELOG.md) for
  anything a user notices.
- Commit messages follow [Conventional Commits](https://www.conventionalcommits.org/)
  (`feat:`, `fix:`, `docs:`, ...).

By contributing you agree that your contribution is licensed under the
[MIT License](LICENSE).

## Releasing (maintainers)

1. Set the version in `project(rbscan VERSION ...)` in `CMakeLists.txt` (the
   Python package takes it from there).
2. Turn the `Unreleased` section of `CHANGELOG.md` into `## [x.y.z] - <date>`.
3. Merge to `main`, then tag `vx.y.z` and push the tag. The pipeline checks
   that the tag matches the version, builds and tests on both platforms,
   publishes the SDKs and wheels to the GitLab Package Registry, creates the
   GitLab release and uploads wheels and sdist to PyPI (Trusted Publishing).
   PyPI takes every version only once: for a trial, run the manual
   `publish:testpypi` job first.
