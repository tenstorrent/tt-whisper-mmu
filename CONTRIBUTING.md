# Contributing to tt-whisper-mmu

Thank you for your interest in contributing. This document explains how to
report problems and submit changes.

## Code of Conduct

This project adheres to the [Contributor Covenant](CODE_OF_CONDUCT.md). By
participating, you are expected to uphold this code. Please report unacceptable
behavior to **ospo@tenstorrent.com**.

## Reporting Bugs

Report bugs by opening a **GitHub Issue**. A good report includes:

- The compiler and version (g++ 11 or later is required).
- A minimal code snippet using the `DvMmu` class that reproduces the problem
  (see the Usage Model in the [README](README.md) and `sample.cpp`).
- The translation inputs (mode, privilege, access type, page-table setup) and
  the expected vs. actual result or exception cause.

Please do **not** file security vulnerabilities as public issues — see
[SECURITY.md](SECURITY.md).

## Submitting Changes

Bug fixes and new functionality are submitted via **Pull Requests**:

1. Create a branch for your change.
2. Keep the change focused; unrelated changes should be separate PRs.
3. Build and run the sample before opening the PR:
   ```
   make          # builds libdvmmu.a
   make sample   # builds the sample program
   ```
4. Open a Pull Request against the default branch with a clear description of
   the motivation and the testing performed.

Pull requests are reviewed on a **weekly** cadence. A maintainer may request
changes before merging. Merges use **squash** only.

## Project-Specific Requirements

- **License headers:** Every Tenstorrent-authored source file must carry an
  SPDX header:
  ```
  // SPDX-License-Identifier: Apache-2.0
  // SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.
  ```
  Do **not** add or modify headers inside the `whisper/` submodule; it is a
  third-party dependency and retains its own upstream license.
- **Scope:** This project is a thin reference-model wrapper around Whisper.
  Changes to translation, PMP, or PMA behavior itself generally belong upstream
  in Whisper; this repository should stay a convenience layer over it.
- **Style:** Match the conventions of the surrounding code.

## License

By contributing, you agree that your contributions will be licensed under the
[Apache License 2.0](LICENSE), consistent with the rest of the project.
