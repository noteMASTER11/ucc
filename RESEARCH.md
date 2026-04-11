# Research Narrative for `removed-SKU-blacklist`

This document reconstructs the branch history relative to `main` and explains the work as a continuous engineering story rather than a raw changelog. It is based on the branch diff and the step-by-step field notes preserved in `Thinking.md`.

At the time of writing, the branch differs from `main` by 10 commits and 29 tracked files, with the center of gravity moving from a simple startup restriction to full practical enablement of Mechrevo YAOSHI hardware.

## 1. The original hypothesis: the software was refusing to trust the machine

The first visible problem looked like a platform lock. UCC was behaving as if the laptop itself was "wrong": the GUI could refuse to start, the daemon could switch into a passive mode, and package dependencies suggested that the whole stack belonged only on a narrow set of officially recognized systems.

The first investigation showed that this was not one single bug but a layered gate:

- a built-in SKU whitelist in `include/Utils.hpp`
- an early GUI exit in `ucc-gui/src/main.cpp`
- passive-mode behavior in `uccd/src/UccDBusService.cpp`
- hard runtime packaging dependencies that implied "no driver, no product"

This is why the first milestone, `3b2f8c3 Remove SKU blacklist`, matters so much. It changed the project's philosophy from "start only on pre-approved machines" to "start on any Linux system, then detect capabilities honestly". That is a subtle but profound shift. Instead of equating unfamiliar hardware with unsupported execution, UCC now launches first and lets real driver and sysfs checks decide what is actually available.

In practical terms, this did four things:

1. removed the artificial SKU gate
2. let the GUI open instead of dying early
3. stopped the daemon from self-disabling just because the DMI string was unfamiliar
4. downgraded `tuxedo-drivers` from a mandatory runtime assumption to an optional or recommended dependency where packaging allows it

This was the moment when the branch stopped being a blacklist-removal experiment and became a broader portability effort.

## 2. Packaging reality check: installability is part of compatibility

After the first gate was removed, the next question was simple: can the branch actually be built and installed like a real package, not just admired as a diff?

That check quickly exposed a packaging bug unrelated to the hardware logic itself. Debian packaging still expected two SVG icons in `usr/share/pixmaps`, while the actual install layout used `hicolor` icons. The result was a build that compiled successfully and then failed during `dh_install`.

The fix landed as `4a46258 Fix Debian package install manifest`. It is small in code size but large in effect. Without it, the branch could not produce a clean `.deb`, which means all later runtime verification would have remained partly hypothetical.

This phase is important in the narrative because it separated two classes of problems:

- policy problems, where the software unnecessarily refused to run
- packaging problems, where the software could run but was not shipped correctly

Only after both were cleared did it make sense to ask the deeper hardware question.

## 3. The real puzzle: why identical-looking Mechrevo hardware still behaved as unsupported

Once the daemon could start and the package could be installed, the branch hit the core research problem. The machine was very close to already supported Uniwill or TUXEDO designs, but key features still did not come alive:

- CPU information was incomplete
- power limits were not available
- keyboard backlight control failed
- `tuxedo_io` did not attach cleanly

The crucial discovery was that the machine was not missing the expected ACPI or WMI surface. The ACPI tables exposed the old Uniwill-style GUID family, and DMI clearly identified the laptop as:

- vendor: `MECHREVO`
- product: `YAOSHI Series`
- board: `YAOSHI Series-X6AR55xY`

In other words, the hardware did not look alien. It looked like a close cousin that the driver stack refused to acknowledge.

That pushed the investigation below UCC itself and into the driver boundary. The notes in `Thinking.md` show the key insight: the failure was not "no interface exists", but "the interface exists and is rejected by compatibility logic". The TUXEDO driver family was accepting certain DMI strings and declining others, even when the platform shape was effectively the same.

## 4. Turning observations into support: UCC plus a documented driver patch

This led to the most consequential hardware-oriented commit: `47587dd Add Mechrevo YAOSHI hardware support`.

This step had two layers.

The first layer was inside UCC itself:

- `uccd/inc/KeyboardBacklightController.hpp` was updated so RGB writes could work correctly across all 126 zones
- `uccd/src/workers/HardwareMonitorWorker.cpp` gained a CPU temperature fallback through `coretemp`, so monitoring would not go blind just because one preferred path was missing
- `uccd/src/UccDBusService.cpp` was adjusted so DBus could correctly accept both profile-shaped keyboard payloads and direct zone arrays sent by the CLI
- `uccd/uccd.service` was widened just enough to permit the daemon to write to the relevant `/sys` paths for LEDs and device control

The second layer was honest documentation of an external requirement. Full Mechrevo functionality did not come from UCC alone. It also required a patch for `tuxedo-drivers 4.21.0`, recorded in:

- `docs/mechrevo-yaoshi-x6ar55xy.md`
- `docs/mechrevo-yaoshi-x6ar55xy-tuxedo-drivers-4.21.0.patch`

That documentation matters because it captures the real engineering boundary. UCC could be taught to speak more clearly, but if the DKMS driver rejected the machine's DMI strings or failed to bind the right TDP tables, no amount of GUI polish would magically create hardware control.

This is the point where the branch becomes scientifically valuable. It stops treating the system as a black box and records a reproducible explanation:

- what the firmware exposes
- what the driver accepts
- what must be patched
- what UCC can already do once the driver path is alive

## 5. From "the hardware is visible" to "profiles really apply"

After the hardware paths were partially recovered, the next bottleneck shifted upward into application logic.

The most revealing bug was almost embarrassingly simple: `libucc-dbus/UccdClient.cpp` contained a stubbed path for CPU ODM power limits that effectively returned failure instead of actually setting values. That meant the UI could show controls while the client layer silently lacked a real write path.

Commit `7b38887 Fix profile TDP controls` addressed this by closing the loop across several layers:

- the client gained a real CPU TDP setter path over DBus
- the daemon learned to accept, clamp, and re-read CPU power limits
- `ProfileSettingsWorker` began applying those limits as real profile state
- `GpuProfileTab` started persisting an explicit `powerLimitW` instead of only an offset
- built-in profiles were treated as immutable on disk but still temporarily applicable to live hardware
- `DashboardTab` gained a one-shot hardware self-check section for CPU, GPU, keyboard backlight, keyboard color, CPU TDP, and GPU TDP

This phase is conceptually important because it transformed several controls from decorative to causal. Before this, the software was already closer to the hardware, but the user experience could still feel misleading: sliders existed, yet the state machine behind them was incomplete.

## 6. NVIDIA power control: when one knob is not enough

Once CPU limits and baseline profile application worked more coherently, attention shifted to the NVIDIA side. The research trail here is especially interesting because it shows a system where one visible control was not enough to move the real hardware outcome.

The branch then evolved through a series of focused commits:

### `37d00df Force NVIDIA cTGP power controls`

This commit strengthened the daemon-side profile application so NVIDIA power control would actively refresh the cTGP-related path instead of assuming that writing one value would be enough.

### `194fdcc Add dGPU stress test button`

A stress button was added in `MonitorTab` to create a repeatable way to provoke meaningful dGPU load and observe whether new power settings held under pressure. This is more than a convenience feature. In this branch, it acts as a research tool embedded into the GUI.

### `1ecfc75 Force aggressive dGPU power profile`

This extended the profile logic toward a more forceful high-power strategy for cases where the normal control path was too conservative.

### `09ab7f6 Keep Mechrevo support without P0 forcing`

The next round of testing showed that always forcing the most aggressive path was not the right default. With a newer NVIDIA driver and `nvidia-powerd`, the system could already reach the right state naturally under load. So the branch deliberately stepped back from making the extreme mode mandatory.

### `299896f Add aggressive dGPU P0 toggle`

Instead of hardcoding the aggressive mode, the branch exposed it as an explicit user-facing toggle in the GPU profile UI. This is a mature outcome: keep the platform support, but let the user opt into the extra shove only when needed.

### `d9bc281 Updated P0 Gpu toggle`

The final UI refinement adjusted the toggle and surrounding profile behavior, making the feature clearer and less intrusive.

Taken together, these commits show a classic engineering pattern. First, force the system to prove it can reach the desired state. Then measure when that force is actually necessary. Finally, convert the discovery into a controllable, explicit option instead of a permanent blunt instrument.

## 7. What had to be researched to reach a real result

The branch succeeded because it did not stop at the first plausible explanation. The work moved through several layers of evidence:

1. source-level gates in UCC itself
2. packaging and install-time assumptions
3. live ACPI, WMI, DMI, sysfs, and DKMS behavior on the target laptop
4. DBus payload formats between CLI, GUI, and daemon
5. profile semantics for CPU and GPU power
6. runtime validation under real service startup and live GPU load

That order matters. If the branch had jumped directly to driver patching, it would have preserved several unnecessary software-level failures. If it had stopped after removing the whitelist, users would have gained launchability but not meaningful control. If it had stopped after the driver patch, users would still have faced inactive or misleading controls.

Success came from treating the system as a stack and debugging each layer on its own terms.

## 8. Final outcome relative to `main`

Relative to `main`, the branch does not merely "support one more laptop". It changes the project's operational model.

UCC now:

- starts on generic Linux systems instead of using a hard pre-launch whitelist
- packages more honestly around optional hardware dependencies
- documents the exact Mechrevo YAOSHI driver gap instead of hiding it
- recovers CPU temperature and keyboard control paths that failed on the target hardware
- exposes a readable hardware self-check on the dashboard
- applies CPU and GPU TDP settings through real end-to-end paths
- gives advanced users an explicit aggressive NVIDIA P0 forcing option instead of making that behavior mandatory
- includes an embedded way to stress the dGPU and observe whether the power strategy really works

In short, this branch began as the removal of a blacklist and ended as a practical field adaptation layer for Mechrevo YAOSHI hardware. The core lesson is simple: compatibility was not blocked by one missing feature, but by a chain of assumptions spread across policy, packaging, drivers, daemon logic, and UI behavior. The branch succeeds because it breaks that chain one link at a time.
