# ChapArm contributor instructions

ChapArm is a Windows-first, seven-DoF right-arm and pen simulator. Commands
express intent; MuJoCo dynamics and contact determine actual movement and pen
pressure. Preserve this distinction throughout the code.

- Use Python 3.11+ for simulation, local agent tools and observation. Use C++17
  and the Windows SDK for the native Wintab provider. Avoid new dependencies
  when the standard library or an existing dependency suffices.
- Keep transport, simulation and native tablet adaptation separate. Units in
  the simulation API are metres, seconds, radians, newtons and newton-metres.
- Never replace system DLLs or modify a user's installed tablet driver.
  Wintab deployment must be scoped to an explicitly selected application.
- Reject nonfinite numbers, unbounded actions and malformed input. The local
  service binds only to loopback. Preserve current state on validation errors.
- Avoid excessive test files. Extend `tests/test_chaparm.py` for Python checks;
  consolidate native ABI/packet checks in `native/tests/wintab_probe.cpp`.
- No unrelated cleanup, generated build files, recordings, credentials, or
  third-party application binaries in git. Document limitations honestly.
- Before submitting, run the existing tests and relevant Windows CI gates.
  A Linux simulation pass is not proof of Krita/Wintab end-to-end compatibility.

Agent operators should read `docs/agent-guide.md`; this file is for contributors.
