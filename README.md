# Caramel Linux

A PS4 Linux kernel for Aeolia, Belize and Baikal consoles, built on
[Strawberry](STRAWBERRY-README.md) by rmux (Dievas). Caramel keeps
Strawberry's patch stack and authorship intact, follows newer upstream
kernels, and adds a few features on top.

Caramel is an independent fork, not endorsed by Strawberry's maintainer.

## What's different from Strawberry
- Rebased onto Linux **7.3.4** (Strawberry 7.3.3).
- `ps4_led.boot_color=<name>` picks the front LED colour at boot.
- Boot logo enabled (one Tux per CPU; 8 seen on a Belize 2).
- Caramel branding in the version string, banner and CI.
- CI builds without a private firmware token.

## Tested
| Console | Build | Result |
| --- | --- | --- |
| Belize 2 (Slim) | Caramel 7.3.4, SlopMax, NoLTO | Boots; display, WiFi and Bluetooth behave like Strawberry 7.3.3; LED colour works |
| Aeolia, Baikal | Caramel | No results yet. Testers wanted |

Not verified: one Baikal-only code path merged during
the rebase (video encode, `amdgpu_job.c`).

## LED colours
Set at boot with `ps4_led.boot_color=pink_blue`, or at runtime by writing 255
to `/sys/class/leds/ps4:<name>:status/brightness`. Names: blue, orange_blue,
orange, orange_white_blue, orange_white, pink_blue, pink, pulsate_orange,
purple, violet_blue, white_blue, white.

On a Belize 2 Slim the names don't always match what you see: `purple` looks
blue, `pink_blue` is the closest to purple, and `pulsate_orange` doesn't
pulsate. Reports from other consoles are welcome.

## Branches
| Branch | Purpose |
| --- | --- |
| `main` | Tested builds |
| `next` | Newest upstream kernel; rebased first, tested second |
| `testing` | Candidates waiting for tester reports |
| `experimental` | Performance and gaming changes |
| `strawberry-upstream` | Untouched Strawberry mirror, for diffing |

## Build
    ./build.sh --option 3 use=SlopMax lto=NoLTO
Profiles: General, Server, SlopMax, Slopium. LTO: NoLTO, ThinLTO, FullLTO.
Output is `out/bzImage`. Or run the **Caramel-LTS** workflow in GitHub Actions.

## Testing
Please report the southbridge, console model, firmware, loader, kernel and
what worked, with a `dmesg`. Aeolia and Baikal reports are the most useful.

## Credits and licence
Strawberry and rmux, whitehax0r, crashniels, feeRnt and everyone credited in
[STRAWBERRY-README.md](STRAWBERRY-README.md). Authorship is preserved on every
commit. GPL-2.0, with source published alongside every build.
