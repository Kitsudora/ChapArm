# ChapArm requirements

ChapArm simulates a person's right arm holding a virtual pen. An agent or human
operator specifies movement intent, observes the resulting drawing, and receives
the arm's actual motion and contact feedback. The primary runtime is Windows;
Krita is the first external painting application to validate.

## Agreed scope

| Area | Requirement |
| --- | --- |
| Arm | Seven independent degrees of freedom: shoulder 3, elbow 1, forearm pronation/supination 1, wrist 2. The original “six-axis” description was a counting error. |
| Grip | Fixed grasp of a rigid pen for the first implementation. Finger articulation is not required. |
| Motion | Smooth but imperfect tracking, with inertia, joint limits, actuator limits, and tunable impedance. Proximal joints should be able to respond more slowly than distal joints. |
| Contact | The canvas physically constrains the pen. Contact force and friction affect the articulated arm, rather than only changing a reported number. |
| Pressure | The painting pressure comes from actual simulated normal contact force. Requested pressure is a controller goal. |
| Control | Joint-level, Cartesian pen-level, and multi-point stroke commands share the same physical model. High-level drawing must not bypass the arm by directly rasterizing the requested path. |
| Feedback | Joint motion, actual and requested pen states, force/contact state, tracking error, load, action status and timestamps are available to the agent. |
| Observation | An internal diagnostic canvas is always available. External framebuffer return is optional and explicitly enabled. Arm visualization and returned still images support inspection. |
| External pen | Provide a native Windows Wintab-compatible application interface for position, contact, pressure and orientation. Krita is the first acceptance target. |
| Agent access | A local persistent service, a JSON-speaking command line, and an MCP interface. Agents can submit, inspect and stop bounded actions without owning the simulation loop. |
| Deployment | Windows setup, build and application-scoped deployment instructions. Do not replace system DLLs or change installed tablet drivers. |
| Documentation | Another agent can install, discover the command interface, run a stroke, interpret force and image feedback, and disable output using repository documentation. |

## Behavioral requirements

1. The simulation advances at a fixed time step independently of model inference.
   A slow cloud response must not stall contact resolution or leave a controller
   waiting to discover that the pen hit the canvas.
2. Commands specify intent. A blocked pen may barely move while its force grows;
   the operator must be able to observe both the force and target error.
3. Impedance stiffness and damping have different meanings. Stiffness determines
   how strongly the arm resists position error; damping opposes velocity. A
   smooth overdamped response is not the same thing as a springy overshoot.
4. Imperfection must remain temporally smooth. Configurable small disturbances
   and unequal joint responses are a controllable approximation, not a claim of
   validated human physiology.
5. The current state persists between commands. Actions must be finite, bounded,
   identifiable and interruptible. Invalid input must leave state unchanged.
6. Feedback includes simulation time and an action identifier. Screen capture
   additionally needs capture timing because external rendering is asynchronous.
7. The output provider must publish pen-up on stop, output disable, and stale
   producer data. Installing a DLL is not proof that the application loaded it;
   report the native consumer handshake separately.
8. Never equate the internal diagnostic canvas with the external application's
   brush rendering. Krita owns its brush, layer and compositing behavior.

## Acceptance and known boundaries

The implementation must demonstrate free motion, canvas blocking, force-dependent
pressure, a completed multi-point stroke, observation images, stopping a stroke,
and malformed-input rejection. Consolidate Python checks in
`tests/test_chaparm.py`; keep native API/packet checks in
`native/tests/wintab_probe.cpp`.

Windows acceptance must additionally build the native provider, pass its ABI and
stream probe, observe an enabled Wintab context from the selected Krita process,
and verify position, variable pressure, pen-up and tilt in Krita's tablet tester
and a drawing document. A Linux simulation test or a native probe does not prove
that Krita loaded the provider or painted a correct stroke.

“Any Wintab software” is a compatibility objective, not an unconditional promise.
Applications vary in DLL loading policy, bitness, context options and supported
packet fields. The first provider is application-scoped user-mode code, not an
installed system tablet driver. Applications that insist on a system-directory
DLL require an application-side loader adaptation or another future integration
route. See [Windows and Krita setup](windows-krita.md).

Full biomechanical identification, finger and brush-bristle deformation, an
OS-enumerated virtual HID driver, physical haptic hardware, automatic brush/layer
selection and continuous native-video model input are outside this initial
implementation. Numerical force feedback and visual force indicators provide the
initial sense of touch.

## Background references

- [Wacom Wintab basics](https://developer-docs.wacom.com/docs/icbt/windows/wintab/wintab-basics/): Wintab is an application API using contexts, notifications and packet queues, not a standalone file or byte-stream format.
- [MuJoCo computation](https://mujoco.readthedocs.io/en/stable/computation/index.html): articulated dynamics and contact constraints provide the simulation substrate.
- [Krita tablet settings](https://docs.krita.org/en/reference_manual/preferences/tablet_settings.html): Krita exposes a Wintab mode and a tablet-event tester on Windows.
