# Implementation verification

This is an initial implementation, with Windows application acceptance still
pending. The source and CI configuration are ready to build; the results below
distinguish checks actually executed from planned checks.

## Executed in the development environment

- Python 3.11 / MuJoCo 3.13 on Linux: `python -m pytest -q` reports
  **10 passed, 1 skipped**. The skipped check loads the native publisher with
  Windows `ctypes`; it requires Windows and a built x64 DLL.
- Checks cover seven-joint dynamics, real canvas contact and pressure, seeded
  imperfections, a complete stroke and lift, action lifecycle and interruption,
  invalid-input rejection without mutation, compliant joint limits, publisher
  failure during stop, HTTP, CLI and a real MCP subprocess with image feedback.
- `python -m pip wheel . --no-deps --wheel-dir dist` builds the Python package,
  including the arm model and local web assets.
- The C++ provider cross-compiles and links for Windows x64 and x86 using Zig's
  Windows GNU target. The x64 native probe also cross-compiles and links.
  Cross-compilation does not execute the Windows APIs or prove MSVC compatibility.
- A three-point stroke with requested force profile 0.5 / 1.0 / 0.5 N completed
  contact, drawing and lift in 5.044 simulated seconds. Its measured peak force
  was 1.136 N. The returned diagnostic ink and arm images were inspected.

## Still required on Windows

1. Run the x64 and Win32 MSVC builds and CTest probe through
   [the build scripts](../scripts/build-native.ps1) or the included Windows CI.
2. Run the Python checks after the x64 build to exercise the Python/native ABI.
3. Inspect the browser dashboard in an interactive local session.
4. Follow [Krita acceptance](windows-krita.md): verify the chosen application
   actually loads this provider, maps the screen rectangle correctly, receives
   variable pressure and orientation, and releases the pen after stop or a
   terminated publisher. The initial candidate is portable Krita 5.2.

The current development environment cannot execute Windows applications. Its
browser also blocked the local preview address. The first attempt to publish
the repository through the connected GitHub app returned
`403 Resource not accessible by integration`; no remote commit or CI run was
created. Repository write access through that connection must be enabled before
publishing and running CI.

## Compatibility remains an acceptance result

This version implements an application-scoped Wintab provider, not an installed
system-wide tablet driver. Applications that load Wintab exclusively from a
Windows system directory will not automatically use it. Universal application
compatibility and live Krita rendering have **not** been demonstrated.
