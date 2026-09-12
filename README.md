# ChapArm

A Windows-first, force-aware virtual **right arm holding a pen**, with a local
operator interface, CLI/MCP control, and a native Wintab provider. The arm has
**seven degrees of freedom**: shoulder 3, elbow 1, forearm rotation 1, wrist 2.

Commands express intent. MuJoCo advances the real joint dynamics and resolves
pen/paper contact. Canvas reaction forces load the arm; actual contact force
determines tablet pressure. Targets are not pasted directly into joint positions
or into the painting application's canvas.

## What is included

- Seven torque-driven joints, joint limits, gravity compensation, finite drive
  strength, compliant tracking, friction and seeded smooth motor imperfections.
- Cartesian, joint, first-contact and complete stroke actions. Strokes support
  pressure profiles, contact acquisition, drawing and lift-off.
- Local feedback: joint motion and loads, desired/actual pen pose, contact force,
  sliding, pressure, timestamped events, action IDs and force history.
- A dependency-free browser dashboard with an orbitable arm view, joint controls,
  pressure/force feedback and a small diagnostic ink canvas.
- Optional native MuJoCo viewer and external Windows canvas capture.
- CLI and MCP tools for asynchronous control and image/state observations.
- `Wintab32.dll`: Wintab contexts, queries, packet queues, coordinates, pressure,
  orientation, proximity and pen-up events; a separate publisher ABI feeds it
  from the simulator. x64 and Win32 build targets and one native probe.

The diagnostic canvas is for checking simulated pen behaviour. **Krita supplies
the professional brush engine.** ChapArm models a rigid grip and spherical nib;
it does not yet simulate individual fingers or flexible brush bristles.

## Quick start: simulator and agent tools

On Windows, install Python 3.11+ and run from the repository:

```powershell
.\scripts\setup.ps1
.\.venv\Scripts\python.exe -m chaparm serve
```

Open <http://127.0.0.1:8765>. Native output starts disabled. The internal simulator
also runs headlessly on Linux for development and deterministic checks.

From another terminal:

```powershell
.\.venv\Scripts\python.exe -m chaparm state
.\.venv\Scripts\python.exe -m chaparm command --file stroke.json
.\.venv\Scripts\python.exe -m chaparm wait ACTION_ID --timeout 30
.\.venv\Scripts\python.exe -m chaparm observe --source canvas --out drawing.png
```

Example `stroke.json` (metres, seconds and newtons):

```json
{
  "op": "stroke",
  "points": [[-0.04, 0.35], [0.0, 0.38], [0.04, 0.35]],
  "duration": 2.0,
  "pressures_n": [0.5, 1.0, 0.5]
}
```

The response contains an action ID. Submission is nonblocking. Only one motion
action runs at a time; observe, poll, or stop it before submitting another.
`move`/`joints` completion means the reference trajectory has finished; inspect
actual tracking error before assuming the arm has settled.

## Windows Wintab and Krita

Install Visual Studio 2022 Build Tools with Desktop development with C++, the
Windows SDK and CMake, then:

```powershell
.\scripts\build-native.ps1 -Architecture x64
```

Follow [Windows/Krita setup](docs/windows-krita.md) to deploy the provider beside
an explicitly selected **portable Krita 5.2-series** executable, calibrate the
visible canvas rectangle, enable output, and run the acceptance checks.

**Compatibility boundary:** Wintab is an application/driver API, not a file format.
This release implements an application-scoped provider; it does not install a
system-wide virtual tablet driver. Krita 5.2's custom backend is the initial
integration target. Applications that explicitly load the system Wintab DLL can
ignore an application-local provider. Newer Krita/Qt versions and other drawing
applications require their own compatibility validation. Do not replace files
in Windows system directories. An ABI probe pass does not by itself prove that
Krita received and painted the stroke.

## Documentation

- [Agreed requirements and acceptance criteria](docs/requirements.md)
- [Dynamics, control, transport and observation architecture](docs/architecture.md)
- [Agent operator guide, HTTP commands and MCP configuration](docs/agent-guide.md)
- [Windows build, deployment and Krita verification](docs/windows-krita.md)
- [Executed checks and pending Windows acceptance](docs/verification.md)
- [Contributor instructions](AGENTS.md)
- [Third-party Wintab header notices](native/THIRD_PARTY_NOTICES.md)

## Verification

```powershell
.\.venv\Scripts\python.exe -m pytest -q
.\scripts\build-native.ps1 -Architecture x64
.\scripts\build-native.ps1 -Architecture Win32
```

Python checks are consolidated in `tests/test_chaparm.py`. Native ABI/IPC checks
are consolidated in `native/tests/wintab_probe.cpp`. CI builds and exercises
both Windows architectures and uploads native artifacts. A GUI session is
required for the separately documented Krita brush/pressure acceptance test.

The physics loop targets 500 Hz of simulation time, and tablet publication runs
at 250 Hz. These are scheduling targets, not hard realtime guarantees. The
model's paper contact is compliant and allows small numerical penetration;
the units, contact limits and remaining modelling boundaries are documented.
