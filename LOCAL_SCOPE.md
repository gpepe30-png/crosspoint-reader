# Fork Scope

This file defines the product scope of this downstream CrossPoint Reader fork.
It overrides the upstream `SCOPE.md` where the two disagree; the upstream file
remains the reference for the upstream project.

## Purpose

This fork expands CrossPoint Reader beyond its reading-focused origin with a
native study app and games. Existing reading functionality and Xteink X4 Classic
support are the foundation to preserve, not work that needs to be re-ported.

## Study

- An Anki-compatible study experience is in scope.
- Prefer importing or downloading deck data to the device so reviews can run
  natively and offline. Do not make network access a prerequisite for studying
  unless the user chooses that design.
- Exact Anki compatibility, supported note/card types, import workflow,
  scheduling behavior, and synchronization are implementation decisions. Do not
  imply full Anki parity or add online services without a concrete requirement.

## Games

Native games are in scope. Specific games, engines, libraries, and multiplayer
or network behavior are not implied by this scope and should be decided per
feature.

## Implementation Boundaries

- Preserve existing reader behavior and supported device profiles, including
  X4 Classic. Do not treat this fork as an X4 Classic port.
- Keep fork-specific features isolated from upstream-owned code where practical.
  Use the CrossPlay and other CrossPoint Reader forks as sources of relevant
  design and implementation methods, adapting them to this tree rather than
  copying them wholesale.
- Before broadening changes into shared upstream code, explain why a
  fork-owned implementation is insufficient and ask when ownership is unclear.
- Implement one user-directed slice at a time. Continue to apply the repository's
  resource, lifecycle, hardware, and verification requirements.