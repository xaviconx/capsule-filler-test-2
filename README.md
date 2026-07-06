# Stepper Loadcell Tester

Banco de pruebas para controlar un NEMA 17 `17HS8401` con un `TMC2209` en modo `STEP/DIR`, leer una celda de carga de `100g` con `HX711`, y experimentar dosificacion de polvo por peso.

## Hardware

Placa inicial: DFRobot Romeo V2.2 / Arduino Leonardo compatible.

Pines por defecto:

| Senal | Pin Romeo/Leonardo |
| --- | --- |
| TMC2209 `STEP` | `A0` |
| TMC2209 `DIR` | `A1` |
| TMC2209 `EN` | `A2` |
| HX711 `DT` | `D9` |
| HX711 `SCK` | `D10` |

Notas:

- Usa fuente externa para el motor. No alimentes el NEMA 17 desde USB ni desde el regulador de la Romeo.
- Une `GND` de la Romeo con `GND` de la fuente del TMC2209.
- Ajusta la corriente del TMC2209 fisicamente antes de probar.
- El firmware arranca con el driver deshabilitado.

## LED integrado

El LED integrado de la placa da feedback visual rapido:

| Estado | Patron |
| --- | --- |
| `idle` / `done` | Pulso corto cada ~1.4 s |
| Movimiento forward | Parpadeo corto encendido / largo apagado |
| Movimiento reverse | Parpadeo largo encendido / corto apagado |
| `step_validation` | Un parpadeo por cada pulso `STEP` |
| `dosing_fast` | Parpadeo rapido |
| `dosing_fine` | Pulso fino lento |
| `settling` | 50/50 lento |
| `error` | Parpadeo muy rapido |

## Firmware

```powershell
cd 11_Codigo\stepper-loadcell-tester
pio run -e romeo_leonardo
pio run -e romeo_leonardo -t upload
pio device monitor -b 115200
```

Comandos seriales principales:

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

El firmware emite un objeto JSON por linea con estado, peso, posicion, velocidad, configuracion y errores.

## Validacion lenta del stepper

Usa este modo para confirmar que `STEP`, `DIR`, `EN`, `VIO` y `GND` estan bien cableados antes de probar movimientos rapidos:

```text
validate 10 forward 500
validate 10 reverse 500
```

Eso manda 10 pulsos `STEP`, separados por 500 ms. El LED integrado prende una vez por cada pulso, asi puedes ver si la Romeo esta mandando pasos aunque el motor no se mueva.

## Backend y UI

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

Abre la URL de Vite. Por default la UI usa `http://127.0.0.1:8001` como backend y corre en el puerto `5174`.

## Modo de dosificacion

`dose target_mg fine_window_mg` usa dos fases:

1. Avance rapido hasta `target_mg - fine_window_mg`.
2. Avance fino por bursts, esperando `settle_ms` despues de cada burst para leer la bascula.

El motor se detiene cuando:

```text
weight_mg >= target_mg - tolerance_mg
```

Si se pasa del target, el estado final sera `done_overshoot`.

## Calibracion

Flujo recomendado con HX711 real:

1. Activa modo real con `scale sim 0`.
2. Deja el plato vacio y ejecuta `tare`.
3. Coloca una pesa de `50g`.
4. Ejecuta `calibrate 50000`.
5. Retira la pesa, ejecuta `tare` de nuevo si hace falta.

El modo simulado esta activo por defecto para probar UI, stepper y algoritmo sin la celda conectada.
