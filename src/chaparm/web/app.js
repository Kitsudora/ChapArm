"use strict";

(() => {
  const $ = (id) => document.getElementById(id);
  const TAU = Math.PI * 2;
  const DEG = 180 / Math.PI;
  const defaults = {
    jointNames: ["Shoulder yaw", "Shoulder pitch", "Shoulder roll", "Elbow flexion", "Forearm rotation", "Wrist flexion", "Wrist deviation"],
    jointLimits: [[-120, 120], [-100, 130], [-100, 100], [0, 150], [-85, 85], [-75, 75], [-35, 35]],
    bounds: { x: [-0.15, 0.15], y: [0.20, 0.50] },
    points: [[0.18, 0, 0.35], [0.21, 0.18, 0.23], [0.025, 0.30, 0.095], [0.012, 0.315, 0.065], [0, 0.33, 0.025]],
  };
  let state = null;
  let initialized = false;
  let source = "internal";
  let planned = [];
  let drawing = false;
  let imageBusy = false;
  let latestImageAt = 0;
  let consecutiveFailures = 0;
  let camera = { azimuth: -1.07, elevation: 0.62, distance: 1.03 };
  let orbit = null;
  const armCanvas = $("arm");
  const armContext = armCanvas.getContext("2d");
  const paperCanvas = $("paper-input");
  const paperContext = paperCanvas.getContext("2d");
  const jointInputs = [];

  const finite = (v, fallback = 0) => Number.isFinite(v) ? v : fallback;
  const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
  const vec = (v, fallback = [0, 0, 0]) => Array.isArray(v) && v.length >= 3 && v.slice(0, 3).every(Number.isFinite) ? v.slice(0, 3) : fallback;
  const add = (a, b) => a.map((v, i) => v + b[i]);
  const sub = (a, b) => a.map((v, i) => v - b[i]);
  const mul = (a, k) => a.map((v) => v * k);
  const dot = (a, b) => a.reduce((s, v, i) => s + v * b[i], 0);
  const length = (a) => Math.sqrt(dot(a, a));
  const unit = (a) => mul(a, 1 / Math.max(length(a), 1e-9));
  const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
  const niceName = (text) => String(text).replaceAll("_", " ").replace(/\b\w/g, (c) => c.toUpperCase());

  function notice(message, error = false) {
    $("notice").textContent = message;
    $("notice").classList.toggle("error", error);
    $("notice").hidden = !message;
  }

  function numeric(id, lo, hi) {
    const input = $(id);
    const value = Number(input.value);
    if (!input.value.trim() || !Number.isFinite(value) || value < lo || value > hi) {
      input.focus();
      throw new Error(`${id.replaceAll("-", " ")} must be between ${lo} and ${hi}.`);
    }
    return value;
  }

  async function command(payload, button) {
    if (button) button.disabled = true;
    try {
      const response = await fetch("/api/command", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload),
        signal: AbortSignal.timeout(5000),
      });
      let result;
      try { result = await response.json(); } catch { throw new Error(`Command failed (HTTP ${response.status}).`); }
      if (!response.ok || result.ok === false || result.error) {
        const error = result.error;
        throw new Error(typeof error === "string" ? error : error?.message || result.message || `Command failed (HTTP ${response.status}).`);
      }
      notice("");
      if (result.state) updateState(result.state);
      return true;
    } catch (error) {
      notice(error.message || "Cannot reach the local simulator.", true);
      return false;
    } finally {
      if (button) button.disabled = false;
    }
  }

  function onCommand(id, makePayload) {
    $(id).addEventListener("click", async () => {
      try { await command(makePayload(), $(id)); } catch (error) { notice(error.message, true); }
    });
  }

  function makeJoints() {
    defaults.jointNames.forEach((name, i) => {
      const row = document.createElement("div");
      row.className = "joint-row";
      const label = document.createElement("label");
      label.htmlFor = `joint-${i}`;
      const index = document.createElement("small");
      index.textContent = String(i + 1).padStart(2, "0");
      const title = document.createElement("span");
      title.textContent = name;
      title.id = `joint-name-${i}`;
      label.append(index, title);
      const input = document.createElement("input");
      input.type = "range";
      input.min = defaults.jointLimits[i][0];
      input.max = defaults.jointLimits[i][1];
      input.step = "0.1";
      input.value = "0";
      input.id = `joint-${i}`;
      const output = document.createElement("output");
      output.htmlFor = input.id;
      output.id = `joint-value-${i}`;
      output.textContent = "0.0°";
      const actual = document.createElement("span");
      actual.className = "joint-actual";
      actual.id = `joint-actual-${i}`;
      actual.textContent = "—";
      input.addEventListener("input", () => { output.textContent = `${Number(input.value).toFixed(1)}°`; });
      row.append(label, input, output, actual);
      $("joints").append(row);
      jointInputs.push(input);
    });
  }

  function syncJoints() {
    const q = state?.q || [];
    jointInputs.forEach((input, i) => {
      if (Number.isFinite(q[i])) {
        input.value = (q[i] * DEG).toFixed(1);
        $(`joint-value-${i}`).textContent = `${Number(input.value).toFixed(1)}°`;
      }
    });
  }

  function syncPosition() {
    if (!state?.tip?.position) return;
    ["x", "y", "z"].forEach((axis, i) => { $(`target-${axis}`).value = (state.tip.position[i] * 1000).toFixed(1); });
  }

  function updateState(next) {
    state = next;
    const tip = vec(state.tip?.position);
    const target = vec(state.target?.position, tip);
    const force = finite(state.contact?.normal_force_n);
    const tangent = state.contact?.tangential_force_n;
    const touching = Boolean(state.contact?.active);
    const paused = state.paused ?? state.running === false;
    $("connection").className = "status online";
    $("connection").lastChild.textContent = "Connected";
    $("pause").textContent = paused ? "Resume" : "Pause";
    $("sim-time").innerHTML = `${finite(state.sim_time).toFixed(2)} <small>s</small>`;
    $("normal-force").innerHTML = `${force.toFixed(3)} <small>N</small>`;
    $("force-meter").style.width = `${clamp(force / 3, 0, 1) * 100}%`;
    $("tangent-force").innerHTML = `${(Array.isArray(tangent) ? length(tangent) : finite(tangent)).toFixed(3)} <small>N</small>`;
    const jointMode = state.target?.mode === "joints" && Array.isArray(state.target?.joint_angles);
    $("target-error-label").textContent = jointMode ? "JOINT ERROR (RMS)" : "TIP TARGET ERROR";
    $("target-error-note").textContent = jointMode ? "Target − actual joint angles" : "Intent − actual position";
    $("target-error").innerHTML = jointMode
      ? `${(length(sub(state.target.joint_angles, state.q)) * DEG / Math.sqrt(7)).toFixed(2)} <small>°</small>`
      : `${(length(sub(target, tip)) * 1000).toFixed(2)} <small>mm</small>`;
    $("pressure").innerHTML = `${(clamp(finite(state.pen?.pressure), 0, 1) * 100).toFixed(1)} <small>%</small>`;
    $("contact-badge").classList.toggle("touching", touching);
    $("contact-badge").querySelector("span").textContent = touching ? "Surface contact" : "Pen lifted";
    $("contact-state").textContent = touching ? (state.contact?.sliding ? "Sliding" : "Touching") : "Air";
    $("contact-state").style.color = touching ? "var(--teal)" : "";
    $("tip-speed").textContent = `${(length(vec(state.tip?.velocity)) * 1000).toFixed(1)} mm/s`;
    $("actual-position").textContent = `Actual: ${tip.map((n) => (n * 1000).toFixed(1)).join(" / ")} mm`;
    $("action-status").textContent = paused ? "Paused" : state.action?.status || "Idle";
    const action = state.action || {};
    $("action-detail").textContent = state.fault || action.reason || action.message || (action.id ? `${action.op || action.kind || "action"} · ${action.stage || ""} · ${Math.round(finite(action.progress) * 100)}%` : "Waiting for a command");
    const output = state.wintab || state.tablet || {};
    $("tablet-state").textContent = typeof output === "string" ? output : output.connected ? "Connected" : output.enabled ? "Publishing" : output.available ? "Ready · disabled" : output.status || "Internal only";
    const hz = state.physics_hz || state.frequency_hz;
    $("loop-status").textContent = `${paused ? "Simulation paused" : "Simulation active"}${hz ? ` · ${hz} Hz physics` : ""}`;
    (state.q || []).slice(0, 7).forEach((angle, i) => {
      $(`joint-actual-${i}`).textContent = `${(finite(angle) * DEG).toFixed(1)}°`;
      $(`joint-actual-${i}`).title = `Drive: ${finite(state.torque?.[i]).toFixed(3)} N·m; contact load: ${finite(state.contact_torque?.[i]).toFixed(3)} N·m${state.drive_saturated?.[i] ? "; drive limit reached" : ""}`;
      $(`joint-actual-${i}`).style.color = state.drive_saturated?.[i] ? "var(--danger)" : "";
    });
    if (!initialized) {
      (state.joint_names || []).slice(0, 7).forEach((name, i) => { $(`joint-name-${i}`).textContent = niceName(name); });
      const limits = state.joint_limits || state.limits;
      if (Array.isArray(limits)) limits.slice(0, 7).forEach((limit, i) => {
        if (Array.isArray(limit) && limit.length === 2) {
          jointInputs[i].min = Math.ceil(limit[0] * DEG * 10) / 10;
          jointInputs[i].max = Math.floor(limit[1] * DEG * 10) / 10;
        }
      });
      syncJoints();
      syncPosition();
      initialized = true;
    }
    drawPaper();
  }

  async function poll() {
    try {
      const response = await fetch("/api/state", { cache: "no-store", signal: AbortSignal.timeout(3500) });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const data = await response.json();
      updateState(data.state || data);
      consecutiveFailures = 0;
    } catch {
      consecutiveFailures++;
      $("connection").className = "status offline";
      $("connection").lastChild.textContent = "Disconnected";
      $("loop-status").textContent = "Simulator unavailable · retrying";
    } finally {
      setTimeout(poll, document.hidden ? 700 : consecutiveFailures ? 1000 : 66);
    }
  }

  function resize(canvas) {
    const box = canvas.getBoundingClientRect();
    const ratio = Math.min(window.devicePixelRatio || 1, 2);
    const w = Math.max(1, Math.round(box.width * ratio));
    const h = Math.max(1, Math.round(box.height * ratio));
    if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
    canvas.getContext("2d").setTransform(ratio, 0, 0, ratio, 0, 0);
    return { width: box.width, height: box.height };
  }

  function drawArm() {
    if (document.hidden) { requestAnimationFrame(drawArm); return; }
    const { width, height } = resize(armCanvas);
    const ctx = armContext;
    ctx.clearRect(0, 0, width, height);
    const center = [0.04, 0.22, 0.17];
    const eye = add(center, [Math.cos(camera.azimuth) * Math.cos(camera.elevation) * camera.distance, Math.sin(camera.azimuth) * Math.cos(camera.elevation) * camera.distance, Math.sin(camera.elevation) * camera.distance]);
    const forward = unit(sub(center, eye));
    const right = unit(cross(forward, [0, 0, 1]));
    const up = cross(right, forward);
    const focal = Math.min(width * 1.0, height * 1.55);
    const project = (p) => {
      const d = sub(p, eye);
      const depth = Math.max(0.10, dot(d, forward));
      return [width * 0.5 + dot(d, right) * focal / depth, height * 0.53 - dot(d, up) * focal / depth, focal / depth];
    };
    function line(a, b, color, lineWidth = 1, dash = []) {
      const pa = project(a); const pb = project(b);
      ctx.beginPath(); ctx.moveTo(pa[0], pa[1]); ctx.lineTo(pb[0], pb[1]);
      ctx.strokeStyle = color; ctx.lineWidth = lineWidth; ctx.setLineDash(dash); ctx.stroke(); ctx.setLineDash([]);
    }
    function polygon(points, fill, stroke) {
      ctx.beginPath();
      points.map(project).forEach((p, i) => i ? ctx.lineTo(p[0], p[1]) : ctx.moveTo(p[0], p[1]));
      ctx.closePath(); ctx.fillStyle = fill; ctx.fill();
      if (stroke) { ctx.strokeStyle = stroke; ctx.lineWidth = 1; ctx.stroke(); }
    }
    function label(point, text, color = "#8394a5", dx = 12, dy = -10) {
      const p = project(point); ctx.fillStyle = color; ctx.font = "9px 'Segoe UI', sans-serif"; ctx.fillText(text, p[0] + dx, p[1] + dy);
    }
    const bounds = paperBounds();
    for (let x = -0.45; x <= 0.451; x += 0.05) line([x, -0.1, -0.002], [x, 0.7, -0.002], "#71859a12");
    for (let y = -0.1; y <= 0.701; y += 0.05) line([-0.45, y, -0.002], [0.45, y, -0.002], "#71859a12");
    const corners = [[bounds.x[0], bounds.y[0], 0], [bounds.x[1], bounds.y[0], 0], [bounds.x[1], bounds.y[1], 0], [bounds.x[0], bounds.y[1], 0]];
    polygon(corners, "#e8e3d617", "#ccd2ca55");
    for (let t = 1; t < 6; t++) {
      const x = bounds.x[0] + (bounds.x[1] - bounds.x[0]) * t / 6;
      const y = bounds.y[0] + (bounds.y[1] - bounds.y[0]) * t / 6;
      line([x, bounds.y[0], 0], [x, bounds.y[1], 0], "#d5d5c80d");
      line([bounds.x[0], y, 0], [bounds.x[1], y, 0], "#d5d5c80d");
    }
    label(corners[1], "DRAWING PLANE · Z = 0", "#839294", 6, 16);

    let points = state?.arm_points;
    if (points && !Array.isArray(points)) points = ["shoulder", "elbow", "wrist", "grip", "tip"].map((name) => points[name]).filter(Boolean);
    if (!Array.isArray(points) || points.length < 3) points = defaults.points;
    points = points.map((p) => vec(p));
    const tip = vec(state?.tip?.position, points[points.length - 1]);
    if (length(sub(points[points.length - 1], tip)) > 0.00001) points = [...points, tip];
    const shoulder = points[0];
    line([shoulder[0], shoulder[1], 0], shoulder, "#77889536", 1, [3, 5]);
    polygon([[shoulder[0] - 0.035, shoulder[1] - 0.035, 0], [shoulder[0] + 0.035, shoulder[1] - 0.035, 0], [shoulder[0] + 0.035, shoulder[1] + 0.035, 0], [shoulder[0] - 0.035, shoulder[1] + 0.035, 0]], "#8090a114", "#8090a130");
    for (let i = 0; i < points.length - 1; i++) line([points[i][0], points[i][1], 0], [points[i + 1][0], points[i + 1][1], 0], "#00000035", i < 2 ? 12 : 4);

    const target = vec(state?.target?.position, tip);
    if (state?.target?.mode !== "joints") {
      line(target, tip, "#a0abba60", 1, [3, 4]);
      const targetScreen = project(target);
      ctx.strokeStyle = "#a2acb7"; ctx.lineWidth = 1; ctx.setLineDash([3, 3]);
      ctx.beginPath(); ctx.arc(targetScreen[0], targetScreen[1], 9, 0, TAU); ctx.stroke(); ctx.setLineDash([]);
      ctx.beginPath(); ctx.moveTo(targetScreen[0] - 13, targetScreen[1]); ctx.lineTo(targetScreen[0] - 6, targetScreen[1]); ctx.moveTo(targetScreen[0] + 6, targetScreen[1]); ctx.lineTo(targetScreen[0] + 13, targetScreen[1]); ctx.stroke();
    }

    ctx.lineCap = "round";
    for (let i = 0; i < points.length - 1; i++) {
      const a = project(points[i]); const b = project(points[i + 1]);
      const isPen = i === points.length - 2;
      const radius = isPen ? 2.2 : i < 2 ? 0.018 * (a[2] + b[2]) / 2 : 0.011 * (a[2] + b[2]) / 2;
      const w = clamp(radius, isPen ? 2 : 7, isPen ? 4 : 25);
      line(points[i], points[i + 1], "#080c1180", w + 5);
      line(points[i], points[i + 1], isPen ? "#e1e7e7" : i < 2 ? "#c59169" : "#dfa67b", w);
      if (!isPen) line(points[i], points[i + 1], "#f4c19a55", Math.max(2, w * 0.27));
    }
    ctx.lineCap = "butt";
    points.slice(0, -1).forEach((point, i) => {
      const p = project(point); const radius = clamp((i < 2 ? 0.023 : 0.012) * p[2], 4, 17);
      const gradient = ctx.createRadialGradient(p[0] - radius * 0.35, p[1] - radius * 0.4, radius * 0.1, p[0], p[1], radius);
      gradient.addColorStop(0, "#ffce9f"); gradient.addColorStop(0.6, "#d39d73"); gradient.addColorStop(1, "#8d674f");
      ctx.beginPath(); ctx.arc(p[0], p[1], radius, 0, TAU); ctx.fillStyle = gradient; ctx.fill(); ctx.strokeStyle = "#efbe9433"; ctx.lineWidth = 1; ctx.stroke();
      ctx.beginPath(); ctx.arc(p[0], p[1], radius * .31, 0, TAU); ctx.fillStyle = "#5c4e48"; ctx.fill();
    });
    label(shoulder, "SHOULDER", "#b49c88", 15, -15);
    if (points[1]) label(points[1], "ELBOW", "#b49c88", 16, 3);
    if (points[2]) label(points[2], "WRIST", "#b49c88", -39, -17);
    const p = project(tip);
    ctx.beginPath(); ctx.arc(p[0], p[1], 3, 0, TAU); ctx.fillStyle = state?.contact?.active ? "#7fdfc7" : "#f2eee6"; ctx.fill();
    const force = Math.max(0, finite(state?.contact?.normal_force_n));
    if (force > 0.003) {
      const end = add(tip, [0, 0, clamp(force * .035, .014, .17)]);
      line(tip, end, "#7fdfc7", 2);
      const a = project(tip); const b = project(end); const angle = Math.atan2(b[1] - a[1], b[0] - a[0]);
      ctx.beginPath(); ctx.moveTo(b[0], b[1]); ctx.lineTo(b[0] - 7 * Math.cos(angle - .45), b[1] - 7 * Math.sin(angle - .45)); ctx.moveTo(b[0], b[1]); ctx.lineTo(b[0] - 7 * Math.cos(angle + .45), b[1] - 7 * Math.sin(angle + .45)); ctx.strokeStyle = "#7fdfc7"; ctx.lineWidth = 2; ctx.stroke();
      label(end, `${force.toFixed(2)} N`, "#7fdfc7", 9, 0);
    }
    const origin = [-.30, .04, 0];
    [[0.045, 0, 0], [0, .045, 0], [0, 0, .045]].forEach((axis, i) => { const end = add(origin, axis); line(origin, end, ["#c57974", "#87aa7c", "#718eba"][i], 1.5); label(end, ["X", "Y", "Z"][i], ["#c57974", "#87aa7c", "#718eba"][i], 3, 0); });
    requestAnimationFrame(drawArm);
  }

  function paperBounds() {
    const bounds = state?.canvas?.bounds || state?.canvas_bounds || state?.canvas;
    if (bounds && Array.isArray(bounds.x) && Array.isArray(bounds.y)) return bounds;
    return defaults.bounds;
  }

  function drawPaper() {
    const { width, height } = resize(paperCanvas);
    const ctx = paperContext;
    ctx.clearRect(0, 0, width, height);
    if (source !== "internal") return;
    if (planned.length) {
      ctx.beginPath(); planned.forEach((p, i) => i ? ctx.lineTo(p[0] * width, p[1] * height) : ctx.moveTo(p[0] * width, p[1] * height));
      ctx.strokeStyle = "#429789"; ctx.lineWidth = 1.7; ctx.lineCap = "round"; ctx.setLineDash([4, 4]); ctx.stroke(); ctx.setLineDash([]);
      const first = planned[0]; ctx.beginPath(); ctx.arc(first[0] * width, first[1] * height, 3, 0, TAU); ctx.fillStyle = "#429789"; ctx.fill();
    }
    const pen = state?.pen;
    if (pen?.proximity && Number.isFinite(pen.u) && Number.isFinite(pen.v) && pen.u >= 0 && pen.u <= 1 && pen.v >= 0 && pen.v <= 1) {
      const x = pen.u * width; const y = pen.v * height;
      ctx.beginPath(); ctx.arc(x, y, pen.contact ? 4 : 6, 0, TAU); ctx.strokeStyle = pen.contact ? "#187a68" : "#aa6c39"; ctx.lineWidth = 1.25; ctx.stroke();
      ctx.beginPath(); ctx.moveTo(x - 10, y); ctx.lineTo(x - 4, y); ctx.moveTo(x + 4, y); ctx.lineTo(x + 10, y); ctx.moveTo(x, y - 10); ctx.lineTo(x, y - 4); ctx.moveTo(x, y + 4); ctx.lineTo(x, y + 10); ctx.stroke();
    }
  }

  function refreshImage() {
    if (imageBusy || document.hidden || Date.now() - latestImageAt < 240) return;
    imageBusy = true;
    latestImageAt = Date.now();
    const requestedSource = source;
    const image = $("paper-image");
    const path = source === "internal" ? "/api/canvas.png" : "/api/capture.png";
    image.onload = () => {
      imageBusy = false;
      if (source !== requestedSource) return;
      image.alt = source === "internal" ? "Current internal drawing canvas" : "Captured external drawing application";
      $("capture-message").hidden = true;
    };
    image.onerror = () => {
      imageBusy = false;
      if (source !== requestedSource) return;
      if (source === "external") {
        $("capture-message").textContent = "External capture is unavailable. Configure a drawing window in the local service, then select Capture again.";
        $("capture-message").hidden = false;
        latestImageAt = Date.now() + 3000;
      }
    };
    image.src = `${path}?t=${Date.now()}`;
  }

  function selectSource(next) {
    source = next;
    const internal = source === "internal";
    $("view-internal").classList.toggle("selected", internal);
    $("view-external").classList.toggle("selected", !internal);
    $("view-internal").setAttribute("aria-pressed", String(internal));
    $("view-external").setAttribute("aria-pressed", String(!internal));
    paperCanvas.style.pointerEvents = internal ? "auto" : "none";
    $("paper-hint").textContent = internal ? "Drag on the paper to plan a stroke" : "External window · observation only";
    $("draw-stroke").disabled = !internal || planned.length < 2;
    $("clear-plan").disabled = !internal || !planned.length;
    $("capture-message").hidden = internal;
    if (!internal) $("capture-message").textContent = "Loading external window capture…";
    latestImageAt = 0;
    drawPaper();
    refreshImage();
  }

  function paperPoint(event) {
    const rect = paperCanvas.getBoundingClientRect();
    return [clamp((event.clientX - rect.left) / rect.width, 0, 1), clamp((event.clientY - rect.top) / rect.height, 0, 1)];
  }

  makeJoints();
  onCommand("move", () => ({ op: "move", position: [numeric("target-x", -200, 250), numeric("target-y", 150, 550), numeric("target-z", -10, 300)].map((v) => v / 1000), duration: numeric("duration", .1, 30) }));
  onCommand("apply-joints", () => ({ op: "joints", angles: jointInputs.map((input) => Number(input.value) / DEG), duration: numeric("duration", .1, 30) }));
  onCommand("approach", () => ({ op: "approach", speed: .01, max_distance: .05 }));
  onCommand("stop", () => ({ op: "stop" }));
  onCommand("reset", () => ({ op: "reset" }));
  onCommand("pause", () => ({ op: "pause", paused: !(state?.paused ?? state?.running === false) }));
  $("use-actual").addEventListener("click", syncPosition);
  $("sync-joints").addEventListener("click", syncJoints);
  $("force").addEventListener("input", () => { $("desired-force").textContent = Number($("force").value).toFixed(2); });
  $("camera-reset").addEventListener("click", () => { camera = { azimuth: -1.07, elevation: .62, distance: 1.03 }; });
  $("view-internal").addEventListener("click", () => selectSource("internal"));
  $("view-external").addEventListener("click", () => selectSource("external"));
  $("clear-plan").addEventListener("click", () => { planned = []; $("clear-plan").disabled = true; $("draw-stroke").disabled = true; $("paper-hint").textContent = "Drag on the paper to plan a stroke"; drawPaper(); });
  $("draw-stroke").addEventListener("click", async () => {
    if (planned.length < 2) return;
    const bounds = paperBounds();
    const points = planned.map(([u, v]) => [bounds.x[0] + u * (bounds.x[1] - bounds.x[0]), bounds.y[1] - v * (bounds.y[1] - bounds.y[0])]);
    try {
      const success = await command({ op: "stroke", points, duration: numeric("duration", .1, 30), pressure_n: Number($("force").value) }, $("draw-stroke"));
      if (success) { planned = []; $("clear-plan").disabled = true; $("draw-stroke").disabled = true; $("paper-hint").textContent = "Drag on the paper to plan a stroke"; drawPaper(); }
    } catch (error) { notice(error.message, true); }
  });

  armCanvas.addEventListener("pointerdown", (event) => { if (event.button !== 0) return; orbit = [event.clientX, event.clientY]; armCanvas.setPointerCapture(event.pointerId); });
  armCanvas.addEventListener("pointermove", (event) => {
    if (!orbit) return;
    camera.azimuth -= (event.clientX - orbit[0]) * .008;
    camera.elevation = clamp(camera.elevation + (event.clientY - orbit[1]) * .008, .10, 1.45);
    orbit = [event.clientX, event.clientY];
  });
  armCanvas.addEventListener("pointerup", () => { orbit = null; });
  armCanvas.addEventListener("pointercancel", () => { orbit = null; });
  armCanvas.addEventListener("wheel", (event) => { event.preventDefault(); camera.distance = clamp(camera.distance * Math.exp(event.deltaY * .001), .55, 2.1); }, { passive: false });
  paperCanvas.addEventListener("pointerdown", (event) => {
    if (source !== "internal" || event.button !== 0) return;
    drawing = true; planned = [paperPoint(event)]; paperCanvas.setPointerCapture(event.pointerId); drawPaper();
  });
  paperCanvas.addEventListener("pointermove", (event) => {
    if (!drawing || planned.length >= 256) return;
    const point = paperPoint(event); const last = planned[planned.length - 1];
    if (Math.hypot(point[0] - last[0], point[1] - last[1]) < .005) return;
    planned.push(point); drawPaper();
  });
  function finishStroke(event) {
    if (!drawing) return;
    if (planned.length < 256) planned.push(paperPoint(event));
    drawing = false;
    $("clear-plan").disabled = !planned.length;
    $("draw-stroke").disabled = planned.length < 2;
    $("paper-hint").textContent = `${planned.length} waypoints · ready to draw`;
    drawPaper();
  }
  paperCanvas.addEventListener("pointerup", finishStroke);
  paperCanvas.addEventListener("pointercancel", () => { drawing = false; planned = []; drawPaper(); });
  document.addEventListener("keydown", (event) => {
    if (event.repeat) return;
    if (event.key === "Escape") { event.preventDefault(); command({ op: "stop" }); }
    else if (event.code === "Space" && !["INPUT", "TEXTAREA", "BUTTON"].includes(document.activeElement.tagName)) {
      event.preventDefault(); command({ op: "pause", paused: !(state?.paused ?? state?.running === false) });
    }
  });
  new ResizeObserver(drawPaper).observe(paperCanvas.parentElement);
  setInterval(refreshImage, 250);
  poll();
  requestAnimationFrame(drawArm);
})();
