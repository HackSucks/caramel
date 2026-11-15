# Caramel Linux

A PS4 Linux kernel tree based on **Strawberry Linux** by Dievas (rmux), which
builds on the work of the PS4 Linux community. Caramel keeps the Strawberry
patch stack, tracks newer upstream kernels, and adds experiments on top.

Caramel is an independent fork. It is not endorsed by Strawberry's
maintainers.

## Status
Early. Only the Strawberry 7.3.3 base has been verified by the maintainer
of this fork. Everything else is untested until listed below.

## Branches
| Branch | Purpose |
| --- | --- |
| `main` | Strawberry 7.3.3, unchanged |
| `strawberry-upstream` | Untouched mirror of Strawberry, for diffing |
| `next` | Strawberry's PS4 stack rebased onto upstream 7.3.4 |
| `experimental`, `testing` | Planned |

## Tested
| Console | Kernel | Result |
| --- | --- | --- |
| Belize 2 | Strawberry 7.3.3, SlopMax | Works well |
| Belize 2 | Caramel `next` | Not yet tested |
| Aeolia, Baikal | Caramel | No results, testers wanted |

## Build
    ./build.sh --option 3 use=SlopMax lto=NoLTO
`./build.sh --help` lists profiles. NoLTO is for quick test builds.

## More
Features, known issues and the full credits are in
[STRAWBERRY-README.md](STRAWBERRY-README.md). Those are Strawberry's claims,
not independently verified by Caramel. Licence: GPL-2.0.
