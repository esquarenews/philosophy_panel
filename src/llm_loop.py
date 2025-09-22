#!/usr/bin/env python3
import os, time, re, sys, subprocess
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

# Choose exactly one transport: HTTP or USB
ESP32_URL  = os.getenv("ESP32_URL")            # e.g. "http://172.20.10.5/post"
SERIAL_PORT= os.getenv("SERIAL_PORT")          # e.g. "/dev/cu.usbserial-0001"
BAUD       = int(os.getenv("SERIAL_BAUD", "115200"))

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
    first = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"[datetime.utcnow().minute % 26]
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

def main():
    print("Script:", __file__, "| Transport:", "HTTP" if os.getenv("ESP32_URL") else "USB", "| Model:", MODEL, "| Host:", OLLAMA_HOST, "| API+CLI fallback", flush=True)
    if not ESP32_URL and not SERIAL_PORT:
        print("Set ESP32_URL (HTTP) or SERIAL_PORT (USB) environment variable.")
        sys.exit(1)

    if SERIAL_PORT and serial is not None:
        # Pre-open serial to avoid per-iteration resets
        try:
            send_serial("")
        except Exception as e:
            print("Warning: initial serial open failed:", e, flush=True)

    while True:
        try:
            raw = ollama_generate()
            line_count = sum(1 for ln in raw.splitlines() if ln.strip() != "")
            print(f"Model lines (non-empty): {line_count}", flush=True)

            wrapped = wrap_to_width(raw, PANEL_COLS)
            payload = wrapped if wrapped.endswith("\n") else wrapped + "\n"
            print("Generated (wrapped to", PANEL_COLS, "cols):\n" + payload, flush=True)

            if ESP32_URL:
                resp = send_http(payload)
                print("HTTP ->", resp, flush=True)
            else:
                resp = send_serial(payload)
                print("SERIAL ->", resp, flush=True)

        except Exception as e:
            print("Error:", e, flush=True)

        time.sleep(INTERVAL_S)

if __name__ == "__main__":
    main()
