# Implementation verification

This record separates executed checks from remaining compatibility work.
Local Windows results below were collected on **2026-09-12** after
restoring the existing history from `ChapArm.bundle`.

## Executed on Windows

Environment: Windows 11 25H2, build 26200.9445; Python **3.13.7** x64,
MuJoCo **3.13.0**, NumPy **2.5.3**, Pillow **12.3.0**.

| Check | Observed result |
| --- | --- |
| MSVC x64 build and CTest | Final provider, launcher and existing native probe built; **1/1 passed**. |
| MSVC Win32 build and CTest | Final provider, launcher and existing native probe built; **1/1 passed**. |
| Python integration checks | `python -m pytest -q --basetemp out/acceptance/pytest-final-<unique-id>`: **12 passed, 0 skipped**, including the Windows `ctypes` publisher and deployment/removal from a path containing spaces, Chinese characters and an emoji. |
| Python wheel | `chaparm-0.1.0-py3-none-any.whl` built; the XML arm model and all local web resources were verified inside it. |
| Browser dashboard | Real headless Chrome controlled through CDP: **6 acceptance checks passed**, with screenshots and state observations. |
| Deployment protections | **16 checks passed** using temporary deployment targets. |

The final Python run used a fresh temporary directory inside the workspace;
the default pytest temporary directory belonged to a different Windows account.

The existing native probe exercised ANSI/Unicode APIs, context operations,
scaling, pressure, orientation, queues, cross-process publication and watchdog
release. The Win32 build exposed an MSVC import-library issue with explicit
decorated aliases; export generation was corrected while preserving public
names and ordinals. These checks execute Windows APIs and supersede the
handoff's cross-compilation-only evidence.

The dashboard checks loaded the live page and seven joint controls, submitted
joint targets, paused and resumed a stroke without replacing its action,
completed a pointer-planned stroke, cancelled another stroke with Stop, and
reported no uncaught JavaScript exceptions. The completed stroke measured a
peak force of **1.10879 N**, ended with zero pressure and no contact, and changed
**2,962 pixels** on the diagnostic canvas. This proves the browser/simulator
path; it does not establish external Krita painting.

Local browser evidence is in `out/acceptance/dashboard/report.json`,
`pixel-diff.json` and the numbered screenshots. Native test logs are under each
architecture's `native/build/.../Testing/Temporary/`. Generated evidence and
third-party application binaries are intentionally excluded from git.

## Krita 5.2.16: executed application acceptance

Candidate: official **Krita 5.2.16 x64 portable**, revision **7d9aefc**, compiled
and loaded Qt **5.15.7**, running in the interactive Windows console session.

Direct deployment beside the original `krita.exe` failed: the process did not
load ChapArm's `Wintab32.dll` and the publisher reported no consumer context.
The official binary uses Qt's tablet backend, whose `QSystemLibrary` loader
selects the system library path. Krita's custom `QLibrary` source is present in
the source tree but excluded by `USE_QT_TABLET_WINDOWS=ON`. The earlier handoff
inferred too much from that source file; a stock 5.2 portable executable is not
a demonstrated direct-deployment target.
[Krita build selection](https://github.com/KDE/krita/blob/v5.2.16/libs/ui/CMakeLists.txt),
[Qt 5.15.7 loader](https://github.com/qt/qtbase/blob/v5.15.7-lts-lgpl/src/plugins/platforms/windows/qwindowstabletsupport.cpp)

The final built and deployed launcher opened the original `krita.dll!krita_main`
with an empty `.exe.local` file and the provider beside the launcher. A captured
status reported consumer PID **13744**, **1 open context**, **1 enabled context**,
matching foreground PID, consumer age **0 ms**, and **`ready=true`** in session
`acceptance`. Every sample in the drawing trial retained `ready=true`. Output
was disabled in the initial handshake, then enabled for the verified drawing.
Evidence: `out/acceptance/final-krita-drawing-report.json`.

The repository's `chaparm-krita.exe` and `deploy-krita.ps1` formalize that
application-specific route. They add `chaparm-krita.exe`, the empty
`chaparm-krita.exe.local` file and `Wintab32.dll` beside the original Krita
binaries. The empty marker affects this launcher's DLL lookup, so it is scoped
to the selected portable installation. System DLLs, registry settings and
installed tablet drivers are unchanged. Follow the
[deployment instructions](windows-krita.md).

The final loader's `--chaparm-check` passed from a fresh deployment with all
three files present before the first launch. The documented deployment uses
the same empty-file form as the successful painting trial.

The launcher also opened `krita-验收-🎨.kra`; Krita reported the exact document
path in `out/acceptance/final-unicode-open.json`. It forwards the original CRT
`argc/argv`, allowing Qt to recover the Windows Unicode command line in the
same way as Krita's original executable.

A separate native correction uses the pen cursor ID rather than cursor 0,
which Qt classifies as a pointing cursor without pen pressure. The existing
native probe is the home for that regression check.

### Drawing, pressure and hover

The actual Krita document and exported PNG were visually inspected. A continuous
three-point stroke rendered with a thicker middle and fine ends using the
**80 px `b) Basic-5 Size`** pressure-sensitive brush. The requested force profile
was **0.5 / 2.0 / 0.5 N**, with a **5 s** drawing phase. The action completed all
stages in **8.040 simulated seconds**, reported `stroke_complete`, and measured
a peak contact force of **2.031209 N**. It ended with **zero pressure** and
**no contact**.

Krita's canvas received **1,270 positive-pressure tablet events**, with maximum
normalized pressure **0.40617751** and pointer type **Pen**. Tablet press, move
and release events were present. A read-only Qt event filter recorded these
events and returned `False`; it did not synthesize pen events or draw the test
stroke. Krita's brush engine generated the image from ChapArm's native output.

The measured painting rectangle was `[445,363,800,800]` in desktop pixels; the
Krita canvas widget reported device-pixel ratio **1.0** during this trial.
The 1000 × 1000 exported image contained **1,480 dark pixels** (minimum RGB
channel below 128); all nonwhite pixels lay within `[316,330,683,401]`, expressed
as a half-open pixel bounding box. The hover export was pixel-identical to the
blank document. The final pressure-stroke image and screen observation were
both inspected. Local evidence includes `final-krita-variable-pressure.png`,
`final-krita-variable-pressure-screen.png`, `final-krita-hover.png`,
`final-variable-pressure-trace.json`, `final-hover-trace.json` and
`final-krita-drawing-report.json`, all under `out/acceptance/`.

### Tilt and contact release

All **5 checks** in `out/acceptance/krita-release-report.json` passed. Each
release check began with simulated contact and observed a real Krita
`TabletRelease` with zero pressure and no pressed buttons. These tilt and
interruption trials used the earlier launcher prototype with the same provider;
the completed-launcher trial above separately verifies drawing and normal
stroke-end release.

| Trigger | Observed delay to the recorded Krita release | Additional result |
| --- | --- | --- |
| `stop` | 12.66 ms | The stroke was cancelled with `operator_stop`. |
| Disable output | 9.78 ms | The physical action remained running, as specified. |
| Change foreground focus | 663.8 ms | The consumer lost foreground ownership and `ready` became false. |
| Terminate publisher PID 26148 | 518.6 ms | Krita received release after the publisher process was killed. |

These are observations from individual trials, not guaranteed latency bounds.
In the fifth check, a requested **0.16 rad** orientation change about the x axis
changed actual simulated `tilt_y` from **−11.723° to −2.636°**; Krita's observed
y-tilt values spanned **−12° to −2°**. Rotation values were also present, but a
large rotation change and rotation-sensitive brush behavior were not tested.

The initial stock Krita launch also crashed in
`libkritaui.dll!KisInputManager::toolProxy` while constructing its input/view
state. A subsequent launch opened successfully. The cause has not been
established, and restart success is not evidence that the startup crash was
fixed. The first crash trace is retained locally as
`out/acceptance/krita-first-launch-crash.log`.

After acceptance, the test Krita and publisher processes were closed and the
two temporary Krita configuration files were restored to their original absent
state. The original Krita executable and DLL SHA-256 hashes still matched the
official ZIP.

### Still pending

- Large pen rotation and tilt/rotation-sensitive brush rendering. The observed
  tilt packet changes do not establish these additional brush behaviors.
- Repeated launch stability, other Krita versions and other display/scaling
  configurations. A successful launch or context does not cover these cases.
- GitHub submission and Windows Actions results. Local build/test success must
  not be reported as a successful remote workflow run.

## Earlier handoff evidence

Before the Windows work, Linux / Python 3.11 / MuJoCo 3.13 reported
**10 passed, 1 skipped**; the skipped check required Windows and a built x64
provider. The provider cross-compiled for Windows x64/x86 with Zig, and a
three-point diagnostic stroke completed in 5.044 simulated seconds with
1.136 N peak force. These remain historical results, not Krita acceptance.

The handoff's first GitHub app write returned
`403 Resource not accessible by integration`; it created neither a remote
commit nor a CI run. That result describes the earlier connection, not the
permissions or remote state of a new session.

## Compatibility remains an acceptance result

ChapArm implements an application-scoped Wintab provider, not an installed
system-wide tablet driver. Application loading, tablet event interpretation,
pressure-sensitive rendering and release behavior require separate evidence.
Universal drawing-application compatibility has not been demonstrated.
