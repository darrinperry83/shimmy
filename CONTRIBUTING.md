# Contributing to shimmy

Thanks for contributing. This file covers the workflow, branches, and PR process.

## Branching model

We use **main → develop → experimental**:

| Branch | Purpose |
|---|---|
| `main` | stable, tagged releases only |
| `develop` | staging — integration + CI before a release |
| `experimental` | risky/forward work (scanout takeover, Metal renderer) |

## Making a change

1. **Fork** and clone recursively (submodules are required):
   ```sh
   git clone --recursive https://github.com/<you>/shimmy.git
   ```
2. Create a branch from `develop` (or `experimental` for forward work):
   ```sh
   git checkout develop && git checkout -b feature/my-thing
   ```
3. Make focused, well-named commits.
4. Build clean:
   ```sh
   ./build.sh gui && ./build.sh cli
   ```
5. Open a **PR** against `develop`. CI runs automatically.

## Pull request checklist

- [ ] Linked issue (if any)
- [ ] Built clean, no new warnings
- [ ] Tests/docs updated where behavior changed
- [ ] Follows the existing style (C99 + Objective-C/Foundation)

## Review + merge

- PRs require a passing CI and a maintainer review.
- Merge to `develop` with a squash or rebase (no merge commits on the trunk).
- When cutting a release, `develop` → `main`, then tag with semver (`v0.1.0`).

## Repos

- `shimmy` (this) — the shim + install/run scripts.
- `shimmy-rdmetal` (submodule) — the Mesa driver; PRs go to its own repo.
- `shimmy-scanout` (submodule) — the injection; tagged per macOS version.

## Code of conduct

Please read [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).

## License

By contributing, you agree your work is licensed under the [MIT License](LICENSE).

## Commit messages

We use [Conventional Commits](https://www.conventionalcommits.org/). Prefixes: `feat:`, `fix:`,
`docs:`, `chore:`, `refactor:`, `test:`, `ci:`. This drives the automated changelog and
semver (release-please).

## Commit style (Mesa-style)

Write commit subjects as `component: description`, e.g.:

```
wlsrv: composite the hardware cursor surface
rdmetal: wire texture upload through the broker
drmasahi: generate the sound-card sysfs at boot
```

Use the standard trailers to link work:

- `Part-of: #123` — this is part of a larger change/topic.
- `Closes: #123` / `Fixes: #123` — this closes the issue.
- `Reviewed-by: Name <email>` — after review.

One logical change per commit; keep the subject under ~72 characters and put the
"why" in the body.
