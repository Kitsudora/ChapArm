# Windows and Krita setup

Use a local Windows machine for the drawing application, simulation and native
provider. The agent may use a cloud model through a client on that machine.
The application integration described here is experimental until the acceptance
steps below have passed on your specific Krita build.

## Install and build

Prerequisites:

- Windows 10 or 11, with a writable project directory.
- Python 3.11 or newer. A 64-bit Python is the usual choice for MuJoCo.
- CMake 3.20 or newer on `PATH`.
- Visual Studio or Build Tools with Desktop development with C++, the MSVC
  compiler and a Windows SDK.
- A separate portable Krita directory for initial integration experiments.

Run from the repository root in PowerShell:

```powershell
.\scripts\setup.ps1
.\scripts\build-native.ps1 -Architecture x64
.\.venv\Scripts\python.exe -m pytest
```

`setup.ps1` creates `.venv` and installs ChapArm plus development dependencies.
Use `-Python 'C:\Path\To\python.exe'` to choose an interpreter explicitly.
The build script builds and runs the native API/stream probe. The resulting DLL
is `native\build\x64\Release\Wintab32.dll`.

For a 32-bit application, also build `-Architecture Win32`. The Python process
must load a DLL matching Python's bitness; the application loads a copy matching
its own bitness. They may use different builds of the provider because the
shared-memory protocol has a fixed-width layout.

For simulation-only use, the native build and Krita are unnecessary:

```powershell
.\.venv\Scripts\chaparm.exe serve --viewer
```

Open [the local operator panel](http://127.0.0.1:8765/). Omit `--viewer` when no
native MuJoCo window is wanted; the browser panel and PNG observations remain
available. Stop the service with Ctrl+C in its terminal.

## Choose a compatible application loader

The provider is loaded into a selected application's process as `Wintab32.dll`.
It does not install an OS tablet or replace a system driver.

The initial candidate is a **Krita 5.2 portable build using Krita's custom
Wintab backend**. That branch's source loads `wintab32` with `QLibrary`, which
makes an application-local provider a plausible route. This is a source-based
compatibility assessment, not a completed Windows drawing test.
[Krita 5.2 loader source](https://github.com/KDE/krita/blob/krita/5.2/libs/ui/input/wintab/kis_tablet_support_win.cpp)

Do not assume a newer Krita build uses the same loader. Qt's upstream Windows
tablet backend uses `QSystemLibrary`; its loading policy can restrict lookup to
the Windows system directory. An application-local DLL may therefore be ignored.
Such a build needs an application-source loader adaptation to an explicit
ChapArm DLL path, or a future alternative backend. Do not work around this by
replacing a system DLL or changing the machine's DLL search configuration.
[Qt Windows tablet source](https://github.com/qt/qtbase/blob/dev/src/plugins/platforms/windows/qwindowstabletsupport.cpp),
[Qt system-library source](https://github.com/qt/qtbase/blob/dev/src/corelib/plugin/qsystemlibrary.cpp)

## Deploy only to the selected application

Close the portable application first. Supply its exact executable path:

```powershell
.\scripts\deploy-wintab.ps1 `
  -AppExecutable 'C:\Apps\Krita-ChapArm\bin\krita.exe' `
  -DllPath '.\native\build\x64\Release\Wintab32.dll'
```

The script checks the executable/DLL machine types, refuses Windows directories,
and refuses to overwrite an existing provider or deployment record. It writes
the DLL next to the selected executable and records its hash. If a provider
already exists, choose a separate clean portable installation; do not delete a
tablet vendor's file to make the deployment succeed.

Launch Krita from a PowerShell process with the same session name used by
ChapArm. This environment variable applies only to this shell and its children:

```powershell
$env:CHAPARM_SESSION = 'drawing'
Start-Process 'C:\Apps\Krita-ChapArm\bin\krita.exe'
```

Krita must be a newly launched process. An already-running instance may handle
the launch request without inheriting the new environment. Both sides default
to `default` if you do not configure a session.

In Krita, select **Settings → Configure Krita → Tablet Settings → WinTab** and
restart as required. Open a new document and choose a brush with pressure-driven
size or opacity. Krita exposes both API selection and a tablet tester in these
settings; the tester's tablet events include raw pressure.
[Krita tablet settings](https://docs.krita.org/en/reference_manual/preferences/tablet_settings.html)

## Calibrate the visible painting area

Arrange the Krita window and canvas, then identify the painting rectangle in
**desktop pixels**: left, top, width, height. This rectangle is neither document
pixel dimensions nor the outer window frame. Start with a single display and
100% display scaling, an unrotated canvas, and an easily recognizable document.

The numbers below are placeholders. Measure your actual painting area before
enabling output:

```powershell
.\.venv\Scripts\chaparm.exe serve `
  --wintab-dll '.\native\build\x64\Release\Wintab32.dll' `
  --session drawing `
  --screen-rect 300 180 900 900 `
  --capture
```

The full simulated canvas maps to this rectangle. Native output starts
**disabled**. `--capture` enables optional screen observations of the configured
rectangle; omit it if pixel return from Krita is not wanted.

The desktop capture sees occluding windows. Keep Krita visible when inspecting
the returned image. Window movement, canvas zoom/pan/rotation or display-scaling
changes require recalibration. This implementation does not query Krita's
document transform or obtain an offscreen layer image.

## Verify the consumer before painting

Query state in another terminal or through MCP:

```powershell
.\.venv\Scripts\chaparm.exe state
```

Inspect the native output status. Loading a publisher DLL does not imply that
Krita opened a Wintab context. Inspect `wintab.context_count`,
`enabled_context_count`, `consumer_age_ms` and `consumer_pid`. Confirm a recent
enabled context belongs to the selected Krita process. `wintab.ready` additionally
requires that process to own the foreground window; it can be false while you
are typing the state command in a foreground terminal. Poll from an agent or
background client while Krita has focus to check readiness for drawing. Native
output arming is separately reported as `wintab.enabled`. The native probe is
also a consumer: do not mistake a probe process for Krita.

If there is no context, check the session name, process bitness, whether Krita
was restarted, its tablet API setting and its DLL loading policy. Do not report
successful application integration on the basis of a DLL file's presence.

Enable output using the local service or the panel only after calibration:

```powershell
Invoke-RestMethod -Uri 'http://127.0.0.1:8765/api/command' `
  -Method Post -ContentType 'application/json' `
  -Body '{"op":"output","enabled":true}'
```

Follow [the agent guide](agent-guide.md) for motion and observation. Disable
output explicitly after the experiment:

```powershell
Invoke-RestMethod -Uri 'http://127.0.0.1:8765/api/command' `
  -Method Post -ContentType 'application/json' `
  -Body '{"op":"output","enabled":false}'
```

**Keep Krita in the foreground while painting.** The provider gates packets to
the foreground application's contexts and releases contact on focus loss.
An agent using MCP or a background HTTP client can submit commands without
changing focus. When typing commands in a terminal or using the browser panel,
return focus to Krita before the stroke's drawing phase. An enabled context alone
does not mean a background application is receiving contact packets.

## Windows acceptance checklist

Use a temporary drawing document and record the Krita version, Qt/backend,
Windows version, display scaling, DLL architecture and session name.

1. Run the native probe through `build-native.ps1`. It must validate exported
   API behavior, context operations and sample packets.
2. In Krita's tablet tester, map the simulated canvas to the tester's drawing
   area. Confirm tablet events appear and their pressure changes. Mouse-only
   motion is insufficient evidence of pen input. Recalibrate when returning to
   the document.
3. Verify a hover move has zero pressure and draws no ink. Confirm the pen lands,
   produces one continuous line and releases contact at the end.
4. Request several conservative contact-force levels. Confirm **actual** contact
   force changes, native normalized pressure follows it and a pressure-sensitive
   Krita brush responds. Krita's own pressure curve can alter the visual result.
5. Change pen orientation within the model's reachable range; inspect orientation
   data in the tester. Use a tilt-sensitive brush only after packet orientation
   is established.
6. Interrupt a stroke with `stop`. Disable output, then test producer shutdown.
   Each must end contact rather than leave painting pressure held. Also switch
   focus away from Krita and confirm contact is released.
7. Compare an optional `screen` observation with the visible Krita document.
   Check position and pressure behavior after window or display changes before
   using a new calibration.

The automated probe does not validate brush rendering, application loading or
all display configurations. Record unsuccessful cases as compatibility gaps.

## Remove or update the provider

Close the selected Krita process, then remove the recorded provider:

```powershell
.\scripts\deploy-wintab.ps1 `
  -AppExecutable 'C:\Apps\Krita-ChapArm\bin\krita.exe' -Remove
```

Removal checks the deployment record and DLL hash. It refuses to remove a DLL
that changed after deployment. To update ChapArm, remove the old recorded copy,
rebuild and deploy again. The regular system tablet installation is unchanged.
