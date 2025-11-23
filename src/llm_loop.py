#!/usr/bin/env python3
import os, time, re, sys, subprocess
import asyncio
import threading
from concurrent.futures import TimeoutError as FuturesTimeout
from datetime import datetime

import requests

# Optional: only import serial if we use USB
try:
    import serial
except Exception:
    serial = None

    print("Loaded llm_loop.py from:", __file__, flush=True)

OLLAMA_HOST = os.getenv("OLLAMA_HOST", "http://127.0.0.1:11434")
MODEL       = os.getenv("OLLAMA_MODEL", "mistral:7b-instruct")

# Choose exactly one transport: BLE (default), HTTP, or USB
TRANSPORT   = os.getenv("TRANSPORT", "BLE").strip().upper()  # BLE | HTTP | USB
ESP32_URL   = os.getenv("ESP32_URL")            # e.g. "http://172.20.10.5/post"
SERIAL_PORT = os.getenv("SERIAL_PORT")          # e.g. "/dev/cu.usbserial-0001"
BAUD        = int(os.getenv("SERIAL_BAUD", "115200"))

# BLE transport (Nordic UART Service)
BLE_NAME    = os.getenv("BLE_NAME", "MatrixPanel")  # default to firmware name
BLE_ADDRESS = os.getenv("BLE_ADDRESS")               # optional MAC/address to skip scanning
BLE_ENABLED = (TRANSPORT == "BLE") or bool(BLE_NAME or BLE_ADDRESS)
NUS_SERVICE_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
NUS_RX_CHAR_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"

try:
    from bleak import BleakClient, BleakScanner
except Exception:
    BleakClient = None
    BleakScanner = None

INTERVAL_S = int(os.getenv("INTERVAL_S", "60"))

PANEL_COLS = int(os.getenv("PANEL_COLS", "21"))  # 128px wide with 5x7 font ≈ 21 cols

SER_HANDLE = None

PROMPT = """You must obey this FORMAT CONTRACT exactly.

OBJECTIVE
Write one coherent, trite, pseudo-philosophical sentence across EXACTLY 30 tokens.

HARD RULES
- Output no more than 6 (SIX) lines. Print nothing else.
- Each line MUST be less than 20 characters
- Do NOT split or hyphenate words.
- No punctuation, digits, emojis, or symbols.
- The FIRST WORD on line 1 must be a simple concrete noun and MUST NOT be the same as your previous answer.
- The six lines together must read as a single sentence.
- check that there are no more than 6 lines.
- check that there are less than 30 tokens

SELF-CHECK BEFORE YOU PRINT
If any of the rules above are violated. Stop. Re-generate, then print.
END.
"""

def build_prompt():
    # Rotate the required first letter by current UTC minute (A..Z)
    first = "ABCDEFGHIJKLMNOPQRSTUVWY"[datetime.utcnow().minute % 24]
    return PROMPT + f"\nEXTRA RULE: The very first word must start with '{first}'."

def ensure_ollama_up():
    try:
        r = requests.get(f"{OLLAMA_HOST}/api/tags", timeout=3)
        r.raise_for_status()
        return True
    except Exception as e:
        print("Ollama not reachable at", OLLAMA_HOST, "-", e, flush=True)
        return False

def has_model(model_name: str) -> bool:
    try:
        r = requests.get(f"{OLLAMA_HOST}/api/tags", timeout=5)
        r.raise_for_status()
        j = r.json()
        if isinstance(j, dict) and "models" in j and isinstance(j["models"], list):
            for m in j["models"]:
                # entries often look like {"name": "phi3:mini", ...}
                if isinstance(m, dict) and m.get("name") == model_name:
                    return True
        return False
    except Exception:
        return False

def wrap_to_width(text: str, cols: int) -> str:
    words = text.split()
    if not words:
        return ""
    lines = []
    cur = []
    length = 0
    for w in words:
        lw = len(w)
        if lw > cols:
            # word too long to fit: place it on its own line (no hyphenation)
            if cur:
                lines.append(" ".join(cur))
                cur = []
                length = 0
            lines.append(w)
            continue
        if length == 0:
            cur = [w]
            length = lw
        elif length + 1 + lw <= cols:
            cur.append(w)
            length += 1 + lw
        else:
            lines.append(" ".join(cur))
            cur = [w]
            length = lw
    if cur:
        lines.append(" ".join(cur))
    return "\n".join(lines)

def ollama_generate() -> str:
    # Try HTTP /api/chat first; if it 404s or connection fails, fall back to CLI `ollama run`
    try:
        if ensure_ollama_up() and has_model(MODEL):
            r = requests.post(f"{OLLAMA_HOST}/api/chat", json={
                "model": MODEL,
                "messages": [{"role": "user", "content": build_prompt()}],
                "stream": False,
                "options": {
                    "temperature": 0.55,
                    "top_p": 0.9,
                    "top_k": 40,
                    "repeat_penalty": 1.5,
                    "num_predict": 30
                }
            }, timeout=120)
            if r.status_code != 404:
                r.raise_for_status()
                j = r.json()
                if isinstance(j, dict):
                    if "message" in j and isinstance(j["message"], dict) and "content" in j["message"]:
                        return j["message"]["content"]
                    if "response" in j:
                        return j["response"]
                # If HTTP path returned but no content, fall through to CLI
    except Exception as e:
        # Any HTTP-related failure will fall back to CLI
        print("HTTP API path failed (falling back to CLI):", e, flush=True)

    # CLI fallback: `ollama run <model> <prompt>` (auto-pulls model if missing)
    try:
        p = subprocess.run(["ollama", "run", MODEL, PROMPT], capture_output=True, text=True, timeout=180)
        if p.returncode == 0:
            return p.stdout
        raise RuntimeError(f"ollama run failed: {p.stderr.strip()}")
    except FileNotFoundError:
        raise RuntimeError("Ollama CLI not found. Install with: brew install ollama")

def send_http(payload: str):
    if not ESP32_URL:
        raise RuntimeError("ESP32_URL not set")
    r = requests.post(ESP32_URL, data=payload.encode("ascii", "ignore"),
                      headers={"Content-Type":"text/plain"}, timeout=10)
    r.raise_for_status()
    return r.text

def send_serial(payload: str):
    global SER_HANDLE
    if not SERIAL_PORT:
        raise RuntimeError("SERIAL_PORT not set")
    if serial is None:
        raise RuntimeError("pyserial not installed")

    # Open once and keep open to avoid repeated auto-resets
    if SER_HANDLE is None or not SER_HANDLE.is_open:
        SER_HANDLE = serial.Serial(SERIAL_PORT, BAUD, timeout=2)
        # Prevent auto-reset on some USB-UARTs by deasserting DTR/RTS
        try:
            SER_HANDLE.dtr = False
            SER_HANDLE.rts = False
        except Exception:
            pass
        time.sleep(0.2)

    n = SER_HANDLE.write(payload.encode("ascii", "ignore"))
    SER_HANDLE.flush()
    print(f"SERIAL wrote {n} bytes", flush=True)
    return "ok-serial"

class _BleLoop:
    def __init__(self):
        self.loop = asyncio.new_event_loop()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self):
        asyncio.set_event_loop(self.loop)
        self.loop.run_forever()

    def call(self, coro, timeout: float = 15.0):
        fut = asyncio.run_coroutine_threadsafe(coro, self.loop)
        try:
            return fut.result(timeout=timeout)
        except FuturesTimeout:
            fut.cancel()
            raise TimeoutError("BLE operation timed out")

class BlePersistent:
    def __init__(self, name: str | None, address: str | None):
        self.name = name
        self.address = address
        self.target = address
        self.client: BleakClient | None = None
        self.ev = _BleLoop()

    async def _discover_target(self) -> str:
        # If we have a cached target, keep using it. The OS may change IDs, so verify by scan if connect fails.
        if self.target:
            return self.target
        # Try fast filtered scan when available
        try:
            if hasattr(BleakScanner, "find_device_by_filter"):
                def _flt(d, ad):
                    if self.name and (d.name == self.name or (d.name or "").startswith(self.name)):
                        return True
                    uuids = []
                    try:
                        uuids = (ad and ad.service_uuids) or []
                    except Exception:
                        pass
                    return any(str(u).lower() == NUS_SERVICE_UUID.lower() for u in uuids)
                dev = await BleakScanner.find_device_by_filter(_flt, timeout=8.0)
                if dev is not None:
                    self.target = dev.address
                    return self.target
        except Exception:
            pass
        # Fallback: full discover
        devices = await BleakScanner.discover(timeout=8.0)
        # Prefer exact/starts-with name
        for d in devices:
            if self.name and (d.name == self.name or (d.name or "").startswith(self.name)):
                self.target = d.address
                return self.target
        # Fallback: by service UUID in metadata
        for d in devices:
            uuids = []
            try:
                uuids = d.metadata.get("uuids") or []
            except Exception:
                pass
            if any((uuid or "").lower() == NUS_SERVICE_UUID.lower() for uuid in uuids):
                self.target = d.address
                return self.target
        raise RuntimeError(f"BLE target not found (name={self.name!r} address={self.address!r})")

    async def _ensure_connected(self):
        if self.client and self.client.is_connected:
            return
        # First try current target or resolve one
        target = await self._discover_target()
        # Close previous if exists
        if self.client:
            try:
                await self.client.disconnect()
            except Exception:
                pass
            self.client = None
        c = BleakClient(target, timeout=10.0)
        try:
            await c.connect()
        except Exception:
            # The cached address may be stale; rescan by name/UUID and retry once.
            self.target = None
            target = await self._discover_target()
            c = BleakClient(target, timeout=12.0)
            await c.connect()
        if not c.is_connected:
            raise RuntimeError("BLE connect failed")
        self.client = c

    async def _write(self, payload: str):
        await self._ensure_connected()
        data = payload.encode("ascii", "ignore")
        try:
            await self.client.write_gatt_char(NUS_RX_CHAR_UUID, data, response=True)
        except Exception:
            # attempt one reconnect then retry once
            await self._ensure_connected()
            await self.client.write_gatt_char(NUS_RX_CHAR_UUID, data, response=True)

    def connect(self):
        self.ev.call(self._ensure_connected())

    def write(self, payload: str):
        self.ev.call(self._write(payload))

    def close(self):
        try:
            if self.client:
                self.ev.call(self.client.disconnect())
        except Exception:
            pass

_BLE_PERSIST: BlePersistent | None = None

def send_ble(payload: str):
    global _BLE_PERSIST
    if _BLE_PERSIST is None:
        if BleakClient is None or BleakScanner is None:
            raise RuntimeError("bleak not installed. Install with: pip install bleak")
        _BLE_PERSIST = BlePersistent(BLE_NAME, BLE_ADDRESS)
        _BLE_PERSIST.connect()
    _BLE_PERSIST.write(payload)
    return "ok-ble"

def main():
    # Decide transport explicitly; default to BLE only as requested
    transport = TRANSPORT
    if transport not in ("BLE", "HTTP", "USB"):
        transport = "BLE"
    print("Script:", __file__, "| Transport:", transport, "| Model:", MODEL, "| Host:", OLLAMA_HOST, "| API+CLI fallback", flush=True)
    if transport == "BLE" and BleakClient is None:
        print("bleak not installed. Install with: pip install bleak")
        sys.exit(1)
    if transport == "HTTP" and not ESP32_URL:
        print("Set ESP32_URL for HTTP transport.")
        sys.exit(1)
    if transport == "USB" and not SERIAL_PORT:
        print("Set SERIAL_PORT for USB transport.")
        sys.exit(1)

    if transport == "USB" and SERIAL_PORT and serial is not None:
        # Pre-open serial to avoid per-iteration resets
        try:
            send_serial("")
        except Exception as e:
            print("Warning: initial serial open failed:", e, flush=True)
    if transport == "BLE":
        # Establish persistent BLE connection up-front
        try:
            send_ble("")
            print("BLE connected (persistent)", flush=True)
        except Exception as e:
            print("Initial BLE connect failed:", e, flush=True)

    while True:
        try:
            raw = ollama_generate()
            line_count = sum(1 for ln in raw.splitlines() if ln.strip() != "")
            print(f"Model lines (non-empty): {line_count}", flush=True)

            wrapped = wrap_to_width(raw, PANEL_COLS)
            payload = wrapped if wrapped.endswith("\n") else wrapped + "\n"
            print("Generated (wrapped to", PANEL_COLS, "cols):\n" + payload, flush=True)

            if transport == "HTTP":
                resp = send_http(payload)
                print("HTTP ->", resp, flush=True)
            elif transport == "BLE":
                resp = send_ble(payload)
                print("BLE ->", resp, flush=True)
            else:  # USB
                resp = send_serial(payload)
                print("SERIAL ->", resp, flush=True)
        except Exception as e:
            print("Error:", e, flush=True)
        
        time.sleep(INTERVAL_S)

    # On exit, try to close BLE cleanly (normally unreachable)
    if _BLE_PERSIST is not None:
        try:
            _BLE_PERSIST.close()
        except Exception:
            pass

if __name__ == "__main__":
    main()
