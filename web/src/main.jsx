import React, { useEffect, useMemo, useState } from "react";
import { createRoot } from "react-dom/client";
import {
  Activity,
  Cable,
  CircleStop,
  Gauge,
  Play,
  Plug,
  RefreshCw,
  Repeat,
  RotateCcw,
  RotateCw,
  Scale,
  Settings2,
  SlidersHorizontal,
  Target,
  Zap
} from "lucide-react";
import "./styles.css";

const API_BASE = import.meta.env.VITE_API_BASE ?? "http://127.0.0.1:8001";
const WS_BASE = API_BASE.replace(/^http/, "ws");

function App() {
  const [ports, setPorts] = useState([]);
  const [selectedPort, setSelectedPort] = useState("");
  const [status, setStatus] = useState({ state: "disconnected", connected: false, config: {} });
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  const [direction, setDirection] = useState("forward");
  const [controls, setControls] = useState({
    speed_sps: 800,
    fine_speed_sps: 160,
    accel_sps2: 900,
    microsteps: 16,
    steps_per_rev: 3200,
    revolutions: 1,
    target_mg: 500,
    fine_window_mg: 50,
    settle_ms: 600,
    tolerance_mg: 3,
    fine_burst_steps: 80,
    sim_mg_per_step: 0.02,
    bounce_cycles: 4,
    bounce_forward_revs: 0.25,
    bounce_reverse_revs: 0.1,
    variable_min_sps: 80,
    variable_max_sps: 900,
    variable_seconds: 8,
    validation_steps: 10,
    validation_interval_ms: 500,
    calibration_mg: 50000
  });

  const config = status.config ?? {};
  const stateText = status.connected ? status.state ?? "unknown" : "disconnected";
  const weight = Number(status.weight_mg ?? 0);
  const target = Number(config.target_mg ?? controls.target_mg);
  const doseProgress = target > 0 ? Math.min(100, Math.max(0, (weight / target) * 100)) : 0;

  useEffect(() => {
    refreshPorts();
  }, []);

  useEffect(() => {
    const ws = new WebSocket(`${WS_BASE}/ws/status`);
    ws.onmessage = (event) => setStatus(JSON.parse(event.data));
    ws.onerror = () => setError("Could not open backend status WebSocket.");
    return () => ws.close();
  }, []);

  const connectedClass = status.connected ? "connected" : "";

  async function api(path, options = {}) {
    setBusy(true);
    setError("");
    try {
      const response = await fetch(`${API_BASE}${path}`, {
        headers: { "Content-Type": "application/json" },
        ...options
      });
      const data = await response.json();
      if (!response.ok) throw new Error(data.detail ?? "Request failed");
      setStatus(data);
      return data;
    } catch (err) {
      setError(err.message);
      throw err;
    } finally {
      setBusy(false);
    }
  }

  async function refreshPorts() {
    const response = await fetch(`${API_BASE}/api/ports`);
    const data = await response.json();
    setPorts(data.ports ?? []);
    if (!selectedPort && data.ports?.length) {
      setSelectedPort(data.ports[0].device);
    }
  }

  function postJson(path, body = {}) {
    return api(path, { method: "POST", body: JSON.stringify(body) });
  }

  async function sendConfig(key) {
    await postJson("/api/config", { key, value: controls[key] });
  }

  async function sendCoreConfig() {
    for (const key of [
      "speed_sps",
      "fine_speed_sps",
      "accel_sps2",
      "microsteps",
      "steps_per_rev",
      "revolutions",
      "target_mg",
      "fine_window_mg",
      "settle_ms",
      "tolerance_mg",
      "fine_burst_steps",
      "sim_mg_per_step"
    ]) {
      await postJson("/api/config", { key, value: controls[key] });
    }
  }

  async function startDose() {
    const targetMg = Number(controls.target_mg);
    const fineWindowMg = Number(controls.fine_window_mg);

    if (fineWindowMg > targetMg) {
      setError("Fine window mg must be less than or equal to Target mg.");
      return;
    }

    await postJson("/api/config", { key: "target_mg", value: targetMg });
    await postJson("/api/config", { key: "fine_window_mg", value: fineWindowMg });
    await postJson("/api/dose", {
      target_mg: targetMg,
      fine_window_mg: fineWindowMg
    });
  }

  function updateControl(key, value) {
    setControls((current) => ({ ...current, [key]: value }));
  }

  return (
    <main className="app-shell">
      <section className="topbar">
        <div>
          <p className="eyebrow">TMC2209 + HX711 bench</p>
          <h1>Stepper Loadcell Tester</h1>
        </div>
        <div className={`status-pill ${connectedClass}`}>
          <Activity size={18} />
          <span>{stateText}</span>
        </div>
      </section>

      <section className="dashboard">
        <div className="metric">
          <Scale size={22} />
          <div>
            <span>Weight</span>
            <strong>{weight.toFixed(1)} mg</strong>
          </div>
        </div>
        <div className="metric">
          <Target size={22} />
          <div>
            <span>Target</span>
            <strong>{target} mg</strong>
          </div>
        </div>
        <div className="metric">
          <Gauge size={22} />
          <div>
            <span>Speed</span>
            <strong>{Number(status.current_speed_sps ?? 0).toFixed(1)} sps</strong>
          </div>
        </div>
        <div className="metric">
          <Zap size={22} />
          <div>
            <span>Driver</span>
            <strong>{status.driver_enabled ? "Enabled" : "Disabled"}</strong>
          </div>
        </div>
      </section>

      <div className="progress-track">
        <div style={{ width: `${doseProgress}%` }} />
      </div>

      <section className="control-grid">
        <Panel icon={<Cable size={20} />} title="Serial" className="wide">
          <div className="inline-controls">
            <select value={selectedPort} onChange={(event) => setSelectedPort(event.target.value)}>
              {ports.length === 0 && <option value="">No ports found</option>}
              {ports.map((port) => (
                <option key={port.device} value={port.device}>
                  {port.device} - {port.description}
                </option>
              ))}
            </select>
            <button className="icon-button" onClick={refreshPorts} disabled={busy} title="Refresh ports">
              <RefreshCw size={18} />
            </button>
          </div>
          <div className="button-row">
            <button
              className="primary"
              onClick={() => postJson("/api/connect", { port: selectedPort, baudrate: 115200 })}
              disabled={!selectedPort || busy}
            >
              <Plug size={18} />
              Connect
            </button>
            <button onClick={() => api("/api/disconnect", { method: "POST" })} disabled={busy}>
              Disconnect
            </button>
          </div>
        </Panel>

        <div className="panel stop-panel">
          <button className="stop-button" onClick={() => api("/api/stop", { method: "POST" })} disabled={busy}>
            <CircleStop size={34} />
            Stop
          </button>
        </div>

        <Panel icon={<Settings2 size={20} />} title="Stepper config">
          <NumberField label="Speed sps" value={controls.speed_sps} onChange={(v) => updateControl("speed_sps", v)} onBlur={() => sendConfig("speed_sps")} />
          <NumberField label="Fine speed sps" value={controls.fine_speed_sps} onChange={(v) => updateControl("fine_speed_sps", v)} onBlur={() => sendConfig("fine_speed_sps")} />
          <NumberField label="Acceleration" value={controls.accel_sps2} onChange={(v) => updateControl("accel_sps2", v)} onBlur={() => sendConfig("accel_sps2")} />
          <NumberField label="Microsteps" value={controls.microsteps} onChange={(v) => updateControl("microsteps", v)} onBlur={() => sendConfig("microsteps")} />
          <NumberField label="Steps / rev" value={controls.steps_per_rev} onChange={(v) => updateControl("steps_per_rev", v)} onBlur={() => sendConfig("steps_per_rev")} />
          <button onClick={sendCoreConfig} disabled={busy || !status.connected}>
            Apply all
          </button>
        </Panel>

        <Panel icon={<RotateCw size={20} />} title="Manual movement">
          <DirectionSelector value={direction} onChange={setDirection} />
          <NumberField label="Revolutions" value={controls.revolutions} step="0.05" onChange={(v) => updateControl("revolutions", v)} onBlur={() => sendConfig("revolutions")} />
          <div className="button-row">
            <button onClick={() => postJson("/api/enable", { enabled: true })} disabled={busy || !status.connected}>
              Enable
            </button>
            <button onClick={() => postJson("/api/enable", { enabled: false })} disabled={busy || !status.connected}>
              Disable
            </button>
          </div>
          <div className="button-row">
            <button className="primary" onClick={() => postJson("/api/jog", { revolutions: controls.revolutions, direction })} disabled={busy || !status.connected}>
              <Play size={18} />
              Jog
            </button>
            <button onClick={() => postJson("/api/run", { direction })} disabled={busy || !status.connected}>
              Run
            </button>
          </div>
        </Panel>

        <Panel icon={<Zap size={20} />} title="Stepper validation">
          <DirectionSelector value={direction} onChange={setDirection} />
          <div className="field-grid two">
            <NumberField label="Steps" value={controls.validation_steps} onChange={(v) => updateControl("validation_steps", v)} />
            <NumberField label="Interval ms" value={controls.validation_interval_ms} onChange={(v) => updateControl("validation_interval_ms", v)} />
          </div>
          <button
            className="primary"
            onClick={() => postJson("/api/validate-step", {
              steps: controls.validation_steps,
              direction,
              interval_ms: controls.validation_interval_ms
            })}
            disabled={busy || !status.connected}
          >
            Pulse steps
          </button>
          <p className="hint">
            LED blinks once per STEP pulse. Start with 10 steps at 500 ms to debug wiring.
          </p>
        </Panel>

        <Panel icon={<Repeat size={20} />} title="Motion modes">
          <div className="field-grid">
            <NumberField label="Cycles" value={controls.bounce_cycles} onChange={(v) => updateControl("bounce_cycles", v)} />
            <NumberField label="Fwd revs" value={controls.bounce_forward_revs} step="0.05" onChange={(v) => updateControl("bounce_forward_revs", v)} />
            <NumberField label="Rev revs" value={controls.bounce_reverse_revs} step="0.05" onChange={(v) => updateControl("bounce_reverse_revs", v)} />
          </div>
          <button onClick={() => postJson("/api/pattern/bounce", {
            cycles: controls.bounce_cycles,
            forward_revs: controls.bounce_forward_revs,
            reverse_revs: controls.bounce_reverse_revs
          })} disabled={busy || !status.connected}>
            Bounce pattern
          </button>
          <div className="field-grid">
            <NumberField label="Min sps" value={controls.variable_min_sps} onChange={(v) => updateControl("variable_min_sps", v)} />
            <NumberField label="Max sps" value={controls.variable_max_sps} onChange={(v) => updateControl("variable_max_sps", v)} />
            <NumberField label="Seconds" value={controls.variable_seconds} onChange={(v) => updateControl("variable_seconds", v)} />
          </div>
          <button onClick={() => postJson("/api/pattern/variable", {
            min_sps: controls.variable_min_sps,
            max_sps: controls.variable_max_sps,
            seconds: controls.variable_seconds
          })} disabled={busy || !status.connected}>
            Variable speed
          </button>
        </Panel>

        <Panel icon={<Scale size={20} />} title="Loadcell">
          <div className="scale-readout">
            <strong>{weight.toFixed(2)} mg</strong>
            <span>raw {status.scale_raw ?? 0} / {status.scale_ready ? "ready" : "not ready"} / {status.scale_stable ? "stable" : "moving"}</span>
          </div>
          <div className="button-row">
            <button onClick={() => api("/api/tare", { method: "POST" })} disabled={busy || !status.connected}>
              Tare
            </button>
            <button onClick={() => postJson("/api/calibrate", { known_mg: controls.calibration_mg })} disabled={busy || !status.connected}>
              Calibrate 50g
            </button>
          </div>
          <NumberField label="Calibration mg" value={controls.calibration_mg} onChange={(v) => updateControl("calibration_mg", v)} />
          <NumberField label="Sim mg / step" value={controls.sim_mg_per_step} step="0.001" onChange={(v) => updateControl("sim_mg_per_step", v)} onBlur={() => sendConfig("sim_mg_per_step")} />
          <div className="button-row">
            <button className={status.scale_simulated ? "selected" : ""} onClick={() => postJson("/api/scale/sim", { enabled: true })} disabled={busy || !status.connected}>
              Sim on
            </button>
            <button className={!status.scale_simulated ? "selected" : ""} onClick={() => postJson("/api/scale/sim", { enabled: false })} disabled={busy || !status.connected}>
              HX711
            </button>
          </div>
        </Panel>

        <Panel icon={<Target size={20} />} title="Dose to weight">
          <NumberField label="Target mg" value={controls.target_mg} onChange={(v) => updateControl("target_mg", v)} onBlur={() => sendConfig("target_mg")} />
          <NumberField label="Fine window mg" value={controls.fine_window_mg} onChange={(v) => updateControl("fine_window_mg", v)} onBlur={() => sendConfig("fine_window_mg")} />
          <NumberField label="Tolerance mg" value={controls.tolerance_mg} onChange={(v) => updateControl("tolerance_mg", v)} onBlur={() => sendConfig("tolerance_mg")} />
          <NumberField label="Settle ms" value={controls.settle_ms} onChange={(v) => updateControl("settle_ms", v)} onBlur={() => sendConfig("settle_ms")} />
          <NumberField label="Fine burst steps" value={controls.fine_burst_steps} onChange={(v) => updateControl("fine_burst_steps", v)} onBlur={() => sendConfig("fine_burst_steps")} />
          <button className="primary" onClick={startDose} disabled={busy || !status.connected}>
            Start dose
          </button>
        </Panel>
      </section>

      <section className="telemetry">
        {error && <div className="error">{error}</div>}
        <code>{status.last_line || "Waiting for serial data..."}</code>
      </section>
    </main>
  );
}

function Panel({ icon, title, className = "", children }) {
  return (
    <div className={`panel ${className}`}>
      <div className="panel-heading">
        {icon}
        <h2>{title}</h2>
      </div>
      {children}
    </div>
  );
}

function NumberField({ label, value, onChange, onBlur, step = "1" }) {
  return (
    <label className="number-field">
      <span>{label}</span>
      <input type="number" value={value} step={step} onChange={(event) => onChange(Number(event.target.value))} onBlur={onBlur} />
    </label>
  );
}

function DirectionSelector({ value, onChange }) {
  return (
    <div className="segmented">
      <button className={value === "forward" ? "selected" : ""} onClick={() => onChange("forward")} title="Forward">
        <RotateCw size={16} />
        Forward
      </button>
      <button className={value === "reverse" ? "selected" : ""} onClick={() => onChange("reverse")} title="Reverse">
        <RotateCcw size={16} />
        Reverse
      </button>
    </div>
  );
}

createRoot(document.getElementById("root")).render(<App />);
