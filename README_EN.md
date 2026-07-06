# Stepper Loadcell Tester

Bench-test project for driving a NEMA 17 `17HS8401` with a `TMC2209` in `STEP/DIR` mode, reading a `100g` load cell through an `HX711`, and experimenting with weight-based powder dispensing.

## Hardware

Initial board: DFRobot Romeo V2.2 / Arduino Leonardo compatible.

Default pins:

| Signal | Romeo/Leonardo pin |
| --- | --- |
| TMC2209 `STEP` | `A0` |
| TMC2209 `DIR` | `A1` |
| TMC2209 `EN` | `A2` |
| HX711 `DT` | `D9` |
| HX711 `SCK` | `D10` |

Notes:

- Use an external motor supply. Do not power the NEMA 17 from USB or the Romeo regulator.
- Tie Romeo `GND` and TMC2209 power-supply `GND` together.
- Set TMC2209 current physically before testing.
- Firmware boots with the driver disabled.

## Built-in LED

The board's built-in LED provides quick visual feedback:

| State | Pattern |
| --- | --- |
| `idle` / `done` | Short pulse every ~1.4 s |
| Forward motion | Short on / long off blink |
| Reverse motion | Long on / short off blink |
| `step_validation` | One blink per `STEP` pulse |
| `dosing_fast` | Fast blink |
| `dosing_fine` | Slow fine pulse |
| `settling` | Slow 50/50 blink |
| `error` | Very fast blink |

## Firmware

```powershell
cd 11_Codigo\stepper-loadcell-tester
pio run -e romeo_leonardo
pio run -e romeo_leonardo -t upload
pio device monitor -b 115200
```

Main serial commands:

```text
status
stop
enable 0|1
config speed_sps 800
config fine_speed_sps 160
config accel_sps2 900
config microsteps 16
config steps_per_rev 3200
config target_mg 500
config fine_window_mg 50
config settle_ms 600
config tolerance_mg 3
config fine_burst_steps 80
config sim_mg_per_step 0.02
jog 1.0 forward
run reverse
validate 10 forward 500
pattern bounce 4 0.25 0.1
pattern variable 80 900 8
tare
calibrate 50000
scale sim 0|1
dose 500 50
```

The firmware emits one JSON object per line with state, weight, position, speed, config and error data.

## Slow Stepper Validation

Use this mode to confirm that `STEP`, `DIR`, `EN`, `VIO` and `GND` are wired correctly before trying faster motion:

```text
validate 10 forward 500
validate 10 reverse 500
```

This sends 10 `STEP` pulses, spaced 500 ms apart. The built-in LED turns on once per pulse, so you can see whether the Romeo is actually sending steps even if the motor does not move.

## Backend and UI

Backend:

```powershell
cd 11_Codigo\stepper-loadcell-tester\backend
python -m venv .venv
.\.venv\Scripts\Activate.ps1
pip install -r requirements.txt
python app.py
```

Frontend:

```powershell
cd 11_Codigo\stepper-loadcell-tester\web
npm install
npm run dev
```

Open the Vite URL shown in the terminal. By default the UI talks to `http://127.0.0.1:8001` and runs on port `5174`.

## Dose Mode

`dose target_mg fine_window_mg` uses two phases:

1. Fast feed until `target_mg - fine_window_mg`.
2. Fine burst feed, waiting `settle_ms` after every burst before reading the scale.

The motor stops when:

```text
weight_mg >= target_mg - tolerance_mg
```

If the dose overshoots the target, final state is `done_overshoot`.

## Calibration

Recommended real-HX711 flow:

1. Enable real scale mode with `scale sim 0`.
2. Keep the pan empty and run `tare`.
3. Place a `50g` reference weight.
4. Run `calibrate 50000`.
5. Remove the weight and run `tare` again if needed.

Simulated scale mode is enabled by default so the UI, stepper and dosing algorithm can be tested without the load cell connected.
