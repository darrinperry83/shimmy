# Contributing to shimmy

Thanks for your interest. shimmy is early-stage, so contributions are welcome in any form:
bug reports, docs, repro cases, and code.

## Getting started

1. Fork the repo and clone it recursively (the submodules are required):
   ```sh
   git clone --recursive https://github.com/<you>/shimmy.git
   ```
2. Build the shim and read the architecture in the README.
3. Open an issue first for anything non-trivial so we can align before you spend time.

## Development workflow

- Branch from `main`, keep commits focused, and write a clear commit message.
- The shim is built with `./build.sh gui` and `./build.sh cli`.
- The driver (`rdmetal/`) and the injection (`scanout/`) are separate submodules — open their
  PRs in their own repos.

## Code style

- Match the surrounding code (the shim is C99 + Objective-C/Foundation).
- No warnings in new code; run the existing build to confirm a clean result.

## Reporting issues

Use the issue templates. Include:
- macOS version and hardware,
- the exact command you ran,
- the relevant log tail (`/tmp/rdgpu-scanout.log`, the shim stderr, the guest session log).

## License

By contributing, you agree your contribution is licensed under the MIT License (see LICENSE).
