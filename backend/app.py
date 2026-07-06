import asyncio
import json
import os
import threading
import time
from dataclasses import dataclass, field
from typing import Any

import serial
from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel, Field
from serial.tools import list_ports


DEFAULT_BAUDRATE = 115200


class ConnectRequest(BaseModel):
    port: str
    baudrate: int = DEFAULT_BAUDRATE


class ConfigRequest(BaseModel):
    key: str
    value: float | int


class EnableRequest(BaseModel):
    enabled: bool


class JogRequest(BaseModel):
    revolutions: float = Field(gt=0)
    direction: str = "forward"


class RunRequest(BaseModel):
    direction: str = "forward"


class ValidateStepRequest(BaseModel):
    steps: int = Field(gt=0, le=1000)
    direction: str = "forward"
    interval_ms: int = Field(ge=50, le=5000)


class BounceRequest(BaseModel):
    cycles: int = Field(gt=0, le=500)
    forward_revs: float = Field(gt=0)
    reverse_revs: float = Field(ge=0)


class VariableRequest(BaseModel):
    min_sps: float = Field(gt=0)
    max_sps: float = Field(gt=0)
    seconds: int = Field(gt=0, le=600)


class SimRequest(BaseModel):
    enabled: bool


class CalibrateRequest(BaseModel):
    known_mg: int = Field(gt=0)


class DoseRequest(BaseModel):
    target_mg: int = Field(gt=0)
    fine_window_mg: int = Field(ge=0)


@dataclass
class SerialBridge:
    """Thread-backed bridge between HTTP requests and the firmware text protocol."""

    port: str | None = None
    baudrate: int = DEFAULT_BAUDRATE
    connection: serial.Serial | None = None
    last_status: dict[str, Any] = field(default_factory=lambda: {"state": "disconnected"})
    last_line: str = ""
    lock: threading.Lock = field(default_factory=threading.Lock)
    reader_thread: threading.Thread | None = None
    running: bool = False

    def connect(self, port: str, baudrate: int = DEFAULT_BAUDRATE) -> dict[str, Any]:
        self.disconnect()
        with self.lock:
            self.connection = serial.Serial(port=port, baudrate=baudrate, timeout=0.1, write_timeout=1)
            self.port = port
            self.baudrate = baudrate
            self.running = True
            self.last_status = {"state": "connecting", "port": port, "baudrate": baudrate}
            self.reader_thread = threading.Thread(target=self._read_loop, daemon=True)
            self.reader_thread.start()

        time.sleep(0.8)
        self.send("status")
        return self.snapshot()

    def disconnect(self) -> None:
        with self.lock:
            self.running = False
            if self.connection and self.connection.is_open:
                try:
                    self.connection.write(b"stop\n")
                    self.connection.close()
                except serial.SerialException:
                    pass
            self.connection = None
            self.port = None
            self.last_status = {"state": "disconnected"}

    def is_connected(self) -> bool:
        return bool(self.connection and self.connection.is_open)

    def send(self, command: str) -> dict[str, Any]:
        with self.lock:
            if not self.connection or not self.connection.is_open:
                raise RuntimeError("Serial port is not connected")
            self.connection.write(f"{command.strip()}\n".encode("utf-8"))
        return self.snapshot()

    def snapshot(self) -> dict[str, Any]:
        status = dict(self.last_status)
        status["connected"] = self.is_connected()
        status["port"] = self.port
        status["baudrate"] = self.baudrate
        status["last_line"] = self.last_line
        return status

    def _read_loop(self) -> None:
        while self.running:
            try:
                if not self.connection:
                    time.sleep(0.1)
                    continue

                raw = self.connection.readline()
                if not raw:
                    continue

                line = raw.decode("utf-8", errors="replace").strip()
                if not line:
                    continue

                self.last_line = line
                if line.startswith("{") and line.endswith("}"):
                    try:
                        parsed = json.loads(line)
                    except json.JSONDecodeError:
                        continue

                    if "ready" in parsed:
                        self.last_status = {"state": "ready", **parsed}
                    elif "error" in parsed and "state" not in parsed:
                        self.last_status = {**self.last_status, "error": parsed["error"]}
                    else:
                        self.last_status = parsed
            except serial.SerialException as exc:
                self.last_status = {"state": "error", "error": str(exc)}
                self.running = False
                break


bridge = SerialBridge()
app = FastAPI(title="Stepper Loadcell Tester API")

app.add_middleware(
    CORSMiddleware,
    allow_origins=os.environ.get("CORS_ORIGINS", "http://localhost:5174,http://127.0.0.1:5174").split(","),
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)


@app.get("/api/ports")
def get_ports() -> dict[str, Any]:
    return {
        "ports": [
            {"device": port.device, "description": port.description, "hwid": port.hwid}
            for port in list_ports.comports()
        ]
    }


@app.post("/api/connect")
def connect(request: ConnectRequest) -> dict[str, Any]:
    try:
        return bridge.connect(request.port, request.baudrate)
    except serial.SerialException as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc


@app.post("/api/disconnect")
def disconnect() -> dict[str, Any]:
    bridge.disconnect()
    return bridge.snapshot()


@app.get("/api/status")
def status() -> dict[str, Any]:
    if bridge.is_connected():
        bridge.send("status")
    return bridge.snapshot()


@app.post("/api/enable")
def enable(request: EnableRequest) -> dict[str, Any]:
    return send_command(f"enable {1 if request.enabled else 0}")


@app.post("/api/config")
def config(request: ConfigRequest) -> dict[str, Any]:
    return send_command(f"config {request.key} {request.value}")


@app.post("/api/jog")
def jog(request: JogRequest) -> dict[str, Any]:
    return send_command(f"jog {request.revolutions} {normalize_direction(request.direction)}")


@app.post("/api/run")
def run(request: RunRequest) -> dict[str, Any]:
    return send_command(f"run {normalize_direction(request.direction)}")


@app.post("/api/validate-step")
def validate_step(request: ValidateStepRequest) -> dict[str, Any]:
    return send_command(f"validate {request.steps} {normalize_direction(request.direction)} {request.interval_ms}")


@app.post("/api/pattern/bounce")
def pattern_bounce(request: BounceRequest) -> dict[str, Any]:
    return send_command(f"pattern bounce {request.cycles} {request.forward_revs} {request.reverse_revs}")


@app.post("/api/pattern/variable")
def pattern_variable(request: VariableRequest) -> dict[str, Any]:
    if request.max_sps < request.min_sps:
        raise HTTPException(status_code=400, detail="max_sps must be >= min_sps")
    return send_command(f"pattern variable {request.min_sps} {request.max_sps} {request.seconds}")


@app.post("/api/tare")
def tare() -> dict[str, Any]:
    return send_command("tare")


@app.post("/api/calibrate")
def calibrate(request: CalibrateRequest) -> dict[str, Any]:
    return send_command(f"calibrate {request.known_mg}")


@app.post("/api/scale/sim")
def scale_sim(request: SimRequest) -> dict[str, Any]:
    return send_command(f"scale sim {1 if request.enabled else 0}")


@app.post("/api/dose")
def dose(request: DoseRequest) -> dict[str, Any]:
    if request.fine_window_mg > request.target_mg:
        raise HTTPException(status_code=400, detail="fine_window_mg must be <= target_mg")
    return send_command(f"dose {request.target_mg} {request.fine_window_mg}")


@app.post("/api/stop")
def stop() -> dict[str, Any]:
    return send_command("stop")


@app.websocket("/ws/status")
async def websocket_status(websocket: WebSocket) -> None:
    await websocket.accept()
    try:
        while True:
            await websocket.send_json(bridge.snapshot())
            await asyncio.sleep(0.25)
    except WebSocketDisconnect:
        return


def normalize_direction(direction: str) -> str:
    normalized = direction.lower()
    if normalized not in {"forward", "reverse"}:
        raise HTTPException(status_code=400, detail="direction must be forward or reverse")
    return normalized


def send_command(command: str) -> dict[str, Any]:
    try:
        bridge.send(command)
        time.sleep(0.08)
        return bridge.snapshot()
    except RuntimeError as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from exc


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="127.0.0.1", port=int(os.environ.get("PORT", "8001")))
