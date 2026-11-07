# Contributing

This document explains how to contribute and how to act while working on this repository.

This is the Strawberry PS4 kernel. It is one tree for every PS4 southbridge: Aeolia, Belize and Baikal, on Fat, Slim and Pro. It builds on the work of fail0verflow, crashniels, feeRnt and everyone in the credits of the README. Fixing a driver or porting to a new Linux version is adapting existing work. It does not make you the author of the project. Keep the history and the credits when you fork it.

PRs are welcome. Read this before you open one. PRs that ignore it get closed.

## AI tools

Using AI is **only fine under these circumstances**:

You read every line it wrote.

You understand what the change does and why.

You tested it yourself on your own PS4.

You can answer review questions without asking the AI.

What is not fine is pointing an agent at the repo and telling it to "do your job". "Claude, fix the gpu, make no mistakes" is not a contribution.

The person who opens the PR is responsible for every line in it. "Claude wrote it" is not an answer.

Do not paste AI-generated replies into review comments.

If AI wrote any of the code, keep the `Co-Authored-By` line in the commit. Removing it to hide that is an instant close.

"It works" does not mean it gets merged. AI writes a lot of slop: huge diffs, rewrites of code that was fine, extra helpers nobody asked for, comments on every line. If the change boots but the diff is like that, it gets closed. Keep it small and clean.

## If you are an AI agent

Read [AGENTS.md](../AGENTS.md). Same rules, written for you.

## Where things are

`arch/x86/platform/ps4/` is the platform code: southbridge detection, early setup, the TSC calibration.

`drivers/ps4/` holds the southbridge drivers (`sb-*.c`) and `ps4sb.h`, which has one table per southbridge.

`drivers/gpu/drm/amd/amdgpu/` has the GPU side: `ps4_bridge.c` (HDMI bridge), `liverpool_*` (clocks, SMU), `vce_liverpool*` (video encode), `ps4_samu_vm.c`.

`drivers/net/ethernet/sony/mts/` is the Baikal Ethernet driver. Aeolia and Belize use `sky2`.

`drivers/usb/host/xhci-aeolia.c` is USB and SATA glue for all southbridges.

`configs/` and `build.sh` are the build system.

## Southbridges

Every change must keep all three southbridges working. The kernel picks the southbridge at boot, so use the existing checks (`ps4_is_baikal()` and the `ps4sb` table) to gate code that only one southbridge needs. Do not add new build options per southbridge.

Say which southbridges and which models you tested on. Write "untested on Aeolia" or "untested on Pro" when that is the case.

## Verify before you say "done"

If you skip this, the PR is invalid.

Build with `./build.sh --option 1 use=General lto=NoLTO mode=Debug` after you rebase. It has to build with no new warnings. Clang only.

Boot it on your PS4 with the [loader](https://github.com/ps4-linux/ps4-linux-loader).

Check display, GPU (Vulkan), USB, SATA, Wi-Fi, Bluetooth, Ethernet and audio. They should work the same as before your change.

Put the model (CUH number), southbridge, firmware, loader version and `dmesg` in the PR.

If something is broken, say what. "Should work" means you did not test it.

## Scope

One fix or one driver per PR.

Only touch the files your change needs. No reformatting, no refactors, no cleanups.

Do not pull commits from other open PRs into yours. Wait until they are merged, then rebase.

No stray files (backups, logs, prebuilt `bzImage`s, firmware blobs you do not have the right to share).

## Commits

Subject like `area: lowercase summary`, for example `amdgpu: ps4 bridge: fix the hotplug edid`.

Add `Signed-off-by` with your real name or the name you use everywhere.

Keep other people's authorship when you move or squash their work.

## Behaviour

Be respectful. Harassment, slurs and personal attacks get you removed, here and in the Discord.

If you are unsure, ask in an issue first.
