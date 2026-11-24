#!/usr/bin/env python3
"""
===============================================================================
 PIYUSH HOME — Smart IoT Backend Server (Zero-Dependency Python 3 + SQLite)
===============================================================================
 Bridges:
   - Frontend Web Dashboard (piyush_home.html)
   - Main ESP32 Controller (piyush_home_esp32.ino + Sinric Pro)
   - ESP32-CAM CCTV Stream

 Features:
   1. Serves piyush_home.html automatically on http://localhost:5000
   2. Persistent SQLite database (piyush_home.db) for:
        - Sensor telemetry history (DHT22 Temp/Hum, MQ-2, MQ-135, PIR Motion)
        - 5-Channel Relay switch states & custom appliance names
        - 20x4 I2C LCD mode & custom 4x20 messages
        - ESP32 / ESP32-CAM connection settings
        - Timestamped system event logs
   3. Background polling thread that keeps ESP32 + Sinric Pro + Web UI in sync
   4. Built-in realistic simulation mode when ESP32 hardware is offline/unplugged
   5. CSV telemetry export & remote ESP32 WiFi credential reset forwarding

 How to Run:
   1. Place piyush_home_backend.py and piyush_home.html in the same folder.
   2. Run:  python piyush_home_backend.py
   3. Open: http://localhost:5000
===============================================================================
"""

import csv
import io
import json
import os
import random
import sqlite3
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# =============================================================================
# 1. SERVER CONFIGURATION
# =============================================================================
HOST = "0.0.0.0"
PORT = int(os.environ.get("PORT", 5000))
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
DB_PATH = os.path.join(BASE_DIR, "piyush_home.db")
FRONTEND_FILE = os.path.join(BASE_DIR, "piyush_home.html")

ESP32_TIMEOUT_SEC = 2.5
POLL_INTERVAL_SEC = 2.5
MAX_TELEMETRY_ROWS = 2000
MAX_EVENT_ROWS = 200

DEFAULT_SWITCHES = [
    (1, "Living Light", "GPIO 26", 1, "bulb"),
    (2, "Ceiling Fan",  "GPIO 27", 1, "fan"),
    (3, "Bedroom AC",   "GPIO 14", 0, "ac"),
    (4, "Porch Light",  "GPIO 12", 0, "porch"),
    (5, "Water Pump",   "GPIO 13", 0, "pump"),
]

DEFAULT_SETTINGS = {
    "useHardware": "false",
    "esp32BaseUrl": "http://192.168.1.100",
    "esp32CamUrl": "http://192.168.1.101:81/stream",
    "motionArmed": "true",
    "lcdMode": "live",
    "lcdCustomLines": json.dumps([
        "   PIYUSH HOME      ",
        "  WELCOME GUESTS!   ",
        "SMART IOT SYSTEM ON ",
        "   HAVE A NICE DAY  ",
    ]),
}

# =============================================================================
# 2. IN-MEMORY LIVE RUNTIME STATE (THREAD-SAFE)
# =============================================================================
state_lock = threading.Lock()
runtime_state = {
    "temp": 26.4,
    "humidity": 54.0,
    "mq2": 142,
    "mq135": 418,
    "motion": False,
    "motionCount": 0,
    "motionLastTime": "None yet",
    "esp32Online": False,
    "lastSync": None,
    "gasSpikeTicks": 0,
    "airSpikeTicks": 0,
    "motionClearAt": 0.0,
}


# =============================================================================
# 3. SQLITE DATABASE LAYER
# =============================================================================
def get_db_connection():
    """Create a thread-safe SQLite connection with row dictionary access."""
    conn = sqlite3.connect(DB_PATH, timeout=10)
    conn.row_factory = sqlite3.Row
    return conn


def init_database():
    """Initialize SQLite tables and seed default configuration & 5 switches."""
    conn = get_db_connection()
    try:
        cur = conn.cursor()

        # Key-value configuration table
        cur.execute("""
            CREATE TABLE IF NOT EXISTS settings (
                key TEXT PRIMARY KEY,
                value TEXT NOT NULL
            )
        """)

        # 5-Channel Relay Switches table
        cur.execute("""
            CREATE TABLE IF NOT EXISTS switches (
                id INTEGER PRIMARY KEY,
                name TEXT NOT NULL,
                gpio TEXT NOT NULL,
                state INTEGER NOT NULL DEFAULT 0,
                icon TEXT NOT NULL DEFAULT 'bulb',
                updated_at TEXT NOT NULL
            )
        """)

        # Time-series Sensor Telemetry table
        cur.execute("""
            CREATE TABLE IF NOT EXISTS telemetry (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                timestamp TEXT NOT NULL,
                temp REAL NOT NULL,
                humidity REAL NOT NULL,
                mq2 INTEGER NOT NULL,
                mq135 INTEGER NOT NULL,
                motion INTEGER NOT NULL
            )
        """)

        # Persistent System Event Logs table
        cur.execute("""
            CREATE TABLE IF NOT EXISTS events (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                timestamp TEXT NOT NULL,
                level TEXT NOT NULL,
                message TEXT NOT NULL
            )
        """)

        # Seed missing default settings
        for key, val in DEFAULT_SETTINGS.items():
            cur.execute(
                "INSERT OR IGNORE INTO settings (key, value) VALUES (?, ?)",
                (key, val),
            )

        # Seed default 5 relay switches if not yet created
        now_iso = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        for sw_id, name, gpio, sw_state, icon in DEFAULT_SWITCHES:
            cur.execute(
                """
                INSERT OR IGNORE INTO switches (id, name, gpio, state, icon, updated_at)
                VALUES (?, ?, ?, ?, ?, ?)
                """,
                (sw_id, name, gpio, sw_state, icon, now_iso),
            )

        conn.commit()
    finally:
        conn.close()


def get_all_settings():
    """Return parsed dictionary of saved backend settings."""
    conn = get_db_connection()
    try:
        rows = conn.execute("SELECT key, value FROM settings").fetchall()
        raw = {r["key"]: r["value"] for r in rows}
    finally:
        conn.close()

    try:
        lcd_lines = json.loads(raw.get("lcdCustomLines", DEFAULT_SETTINGS["lcdCustomLines"]))
        if not isinstance(lcd_lines, list) or len(lcd_lines) != 4:
            lcd_lines = json.loads(DEFAULT_SETTINGS["lcdCustomLines"])
    except Exception:
        lcd_lines = json.loads(DEFAULT_SETTINGS["lcdCustomLines"])

    return {
        "useHardware": raw.get("useHardware", "false").lower() == "true",
        "esp32BaseUrl": raw.get("esp32BaseUrl", DEFAULT_SETTINGS["esp32BaseUrl"]).rstrip("/"),
        "esp32CamUrl": raw.get("esp32CamUrl", DEFAULT_SETTINGS["esp32CamUrl"]),
        "motionArmed": raw.get("motionArmed", "true").lower() == "true",
        "lcdMode": raw.get("lcdMode", "live"),
        "lcdCustomLines": [pad_lcd20(line) for line in lcd_lines],
    }


def save_setting(key, value):
    """Persist a single setting key/value pair into SQLite."""
    conn = get_db_connection()
    try:
        conn.execute(
            "INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?)",
            (key, str(value)),
        )
        conn.commit()
    finally:
        conn.close()


def get_all_switches():
    """Return the 5 smart switches ordered by channel ID (1..5)."""
    conn = get_db_connection()
    try:
        rows = conn.execute(
            "SELECT id, name, gpio, state, icon, updated_at FROM switches ORDER BY id ASC"
        ).fetchall()
        return [
            {
                "id": r["id"],
                "name": r["name"],
                "gpio": r["gpio"],
                "state": bool(r["state"]),
                "icon": r["icon"],
                "updatedAt": r["updated_at"],
            }
            for r in rows
        ]
    finally:
        conn.close()


def update_switch_state_db(switch_id, turn_on):
    """Update a single relay switch state in SQLite."""
    now_iso = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    conn = get_db_connection()
    try:
        conn.execute(
            "UPDATE switches SET state = ?, updated_at = ? WHERE id = ?",
            (1 if turn_on else 0, now_iso, switch_id),
        )
        conn.commit()
    finally:
        conn.close()


def rename_switch_db(switch_id, new_name):
    """Rename an appliance switch in SQLite."""
    now_iso = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    conn = get_db_connection()
    try:
        conn.execute(
            "UPDATE switches SET name = ?, updated_at = ? WHERE id = ?",
            (new_name[:20], now_iso, switch_id),
        )
        conn.commit()
    finally:
        conn.close()


def insert_telemetry_db(temp, humidity, mq2, mq135, motion):
    """Append a sensor snapshot to SQLite and prune old rows beyond MAX_TELEMETRY_ROWS."""
    now_iso = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    conn = get_db_connection()
    try:
        conn.execute(
            """
            INSERT INTO telemetry (timestamp, temp, humidity, mq2, mq135, motion)
            VALUES (?, ?, ?, ?, ?, ?)
            """,
            (now_iso, round(temp, 1), round(humidity, 1), int(mq2), int(mq135), 1 if motion else 0),
        )
        conn.execute(
            """
            DELETE FROM telemetry WHERE id NOT IN (
                SELECT id FROM telemetry ORDER BY id DESC LIMIT ?
            )
            """,
            (MAX_TELEMETRY_ROWS,),
        )
        conn.commit()
    finally:
        conn.close()


def get_telemetry_history(limit=30):
    """Fetch recent telemetry history ordered oldest-to-newest for charting."""
    safe_limit = max(1, min(int(limit), MAX_TELEMETRY_ROWS))
    conn = get_db_connection()
    try:
        rows = conn.execute(
            """
            SELECT timestamp, temp, humidity, mq2, mq135, motion
            FROM telemetry ORDER BY id DESC LIMIT ?
            """,
            (safe_limit,),
        ).fetchall()
        data = [
            {
                "timestamp": r["timestamp"],
                "temp": r["temp"],
                "humidity": r["humidity"],
                "mq2": r["mq2"],
                "mq135": r["mq135"],
                "motion": bool(r["motion"]),
            }
            for r in reversed(rows)
        ]
        return data
    finally:
        conn.close()


def log_event_db(message, level="info"):
    """Record a system event in SQLite and keep the log table bounded."""
    now_time = datetime.now().strftime("%H:%M:%S")
    conn = get_db_connection()
    try:
        conn.execute(
            "INSERT INTO events (timestamp, level, message) VALUES (?, ?, ?)",
            (now_time, level, str(message)),
        )
        conn.execute(
            """
            DELETE FROM events WHERE id NOT IN (
                SELECT id FROM events ORDER BY id DESC LIMIT ?
            )
            """,
            (MAX_EVENT_ROWS,),
        )
        conn.commit()
    finally:
        conn.close()


def get_recent_events(limit=40):
    """Fetch the latest event log entries."""
    conn = get_db_connection()
    try:
        rows = conn.execute(
            "SELECT id, timestamp, level, message FROM events ORDER BY id DESC LIMIT ?",
            (limit,),
        ).fetchall()
        return [
            {
                "id": r["id"],
                "timestamp": r["timestamp"],
                "level": r["level"],
                "message": r["message"],
            }
            for r in rows
        ]
    finally:
        conn.close()


def clear_events_db():
    """Delete all stored event log rows."""
    conn = get_db_connection()
    try:
        conn.execute("DELETE FROM events")
        conn.commit()
    finally:
        conn.close()


# =============================================================================
# 4. UTILITY & ESP32 HARDWARE BRIDGE FUNCTIONS
# =============================================================================
def pad_lcd20(text):
    """Format any string to strictly 20 characters for the 20x4 I2C LCD."""
    return str(text or "")[:20].ljust(20)


def esp32_http_request(url, method="GET", json_body=None):
    """
    Perform a safe HTTP request to the physical ESP32 controller.
    Returns (success: bool, parsed_data_or_error_str).
    """
    data_bytes = None
    headers = {"User-Agent": "PIYUSH-HOME-Backend/1.0"}
    if json_body is not None:
        data_bytes = json.dumps(json_body).encode("utf-8")
        headers["Content-Type"] = "application/json"

    req = urllib.request.Request(url, data=data_bytes, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=ESP32_TIMEOUT_SEC) as resp:
            raw = resp.read().decode("utf-8", errors="replace")
            try:
                return True, json.loads(raw)
            except json.JSONDecodeError:
                return True, {"raw": raw}
    except Exception as exc:
        return False, str(exc)


def forward_switch_to_esp32(switch_id, turn_on):
    """Forward a relay switch toggle to ESP32 if Hardware Mode is active."""
    settings = get_all_settings()
    if not settings["useHardware"]:
        return True, "simulation"

    url = f"{settings['esp32BaseUrl']}/api/switch?id={switch_id}&state={1 if turn_on else 0}"
    ok, result = esp32_http_request(url, method="GET")
    with state_lock:
        runtime_state["esp32Online"] = ok
    return ok, result


def forward_lcd_to_esp32(lines=None, live_mode=False):
    """Forward either custom 20x4 lines or Live Mode command to the ESP32."""
    settings = get_all_settings()
    if not settings["useHardware"]:
        return True, "simulation"

    if live_mode:
        url = f"{settings['esp32BaseUrl']}/api/lcd/live"
        ok, result = esp32_http_request(url, method="GET")
    else:
        url = f"{settings['esp32BaseUrl']}/api/lcd"
        ok, result = esp32_http_request(url, method="POST", json_body={"lines": lines})

    with state_lock:
        runtime_state["esp32Online"] = ok
    return ok, result


# =============================================================================
# 5. BACKGROUND TELEMETRY & ESP32 SYNC WORKER
# =============================================================================
def telemetry_worker():
    """
    Runs continuously in a background daemon thread:
      - In Hardware Mode: polls ESP32 /api/sensors, syncs Sinric Pro switch changes,
        tracks PIR motion triggers, and stores readings in SQLite.
      - In Simulation Mode: smoothly simulates realistic DHT22, MQ-2, and MQ-135
        telemetry so the full stack works even without physical hardware connected.
    """
    persist_counter = 0

    while True:
        try:
            settings = get_all_settings()
            now_ts = time.time()

            if settings["useHardware"]:
                url = f"{settings['esp32BaseUrl']}/api/sensors"
                ok, data = esp32_http_request(url, method="GET")

                if ok and isinstance(data, dict):
                    prev_motion = False
                    with state_lock:
                        prev_motion = runtime_state["motion"]
                        if isinstance(data.get("temp"), (int, float)):
                            runtime_state["temp"] = float(data["temp"])
                        if isinstance(data.get("humidity"), (int, float)):
                            runtime_state["humidity"] = float(data["humidity"])
                        if isinstance(data.get("mq2"), (int, float)):
                            runtime_state["mq2"] = int(data["mq2"])
                        if isinstance(data.get("mq135"), (int, float)):
                            runtime_state["mq135"] = int(data["mq135"])

                        new_motion = bool(data.get("motion", False))
                        runtime_state["motion"] = new_motion
                        if new_motion and not prev_motion:
                            runtime_state["motionCount"] += 1
                            runtime_state["motionLastTime"] = datetime.now().strftime("%H:%M:%S")
                            log_event_db(
                                f"PIR Motion detected on ESP32 (#{runtime_state['motionCount']})",
                                "danger",
                            )

                        runtime_state["esp32Online"] = True
                        runtime_state["lastSync"] = datetime.now().strftime("%H:%M:%S")

                    # Two-way Sync: If Alexa / Sinric Pro toggled a switch on ESP32, update SQLite!
                    esp_switches = data.get("switches")
                    if isinstance(esp_switches, list) and len(esp_switches) == 5:
                        db_switches = get_all_switches()
                        for idx, esp_val in enumerate(esp_switches):
                            bool_val = bool(esp_val)
                            if db_switches[idx]["state"] != bool_val:
                                update_switch_state_db(idx + 1, bool_val)
                                log_event_db(
                                    f"Synced SW-{idx + 1} ({db_switches[idx]['name']}) -> "
                                    f"{'ON' if bool_val else 'OFF'} from ESP32 / Sinric Pro",
                                    "info",
                                )
                else:
                    with state_lock:
                        runtime_state["esp32Online"] = False
            else:
                # Smooth Simulation Mode
                with state_lock:
                    runtime_state["esp32Online"] = False
                    runtime_state["temp"] = max(
                        18.0, min(40.0, runtime_state["temp"] + (random.random() - 0.5) * 0.4)
                    )
                    runtime_state["humidity"] = max(
                        25.0, min(88.0, runtime_state["humidity"] + (random.random() - 0.5) * 1.4)
                    )

                    if runtime_state["gasSpikeTicks"] > 0:
                        runtime_state["gasSpikeTicks"] -= 1
                        runtime_state["mq2"] = int(460 + random.random() * 90)
                    else:
                        runtime_state["mq2"] = int(
                            runtime_state["mq2"]
                            + (145 - runtime_state["mq2"]) * 0.25
                            + (random.random() - 0.5) * 14
                        )

                    if runtime_state["airSpikeTicks"] > 0:
                        runtime_state["airSpikeTicks"] -= 1
                        runtime_state["mq135"] = int(1080 + random.random() * 140)
                    else:
                        runtime_state["mq135"] = int(
                            runtime_state["mq135"]
                            + (420 - runtime_state["mq135"]) * 0.25
                            + (random.random() - 0.5) * 20
                        )

                    # Auto-clear simulated motion after 6 seconds
                    if runtime_state["motion"] and now_ts >= runtime_state["motionClearAt"]:
                        runtime_state["motion"] = False

                    runtime_state["lastSync"] = datetime.now().strftime("%H:%M:%S")

            # Save telemetry sample to SQLite every 2 cycles (~5 seconds)
            persist_counter += 1
            if persist_counter >= 2:
                persist_counter = 0
                with state_lock:
                    insert_telemetry_db(
                        runtime_state["temp"],
                        runtime_state["humidity"],
                        runtime_state["mq2"],
                        runtime_state["mq135"],
                        runtime_state["motion"],
                    )

        except Exception as exc:
            print(f"[WORKER ERROR] {exc}")

        time.sleep(POLL_INTERVAL_SEC)


# =============================================================================
# 6. HTTP REST API REQUEST HANDLER
# =============================================================================
class PiyushHomeHandler(BaseHTTPRequestHandler):
    """Handles all Frontend static serving and REST API routes."""

    def log_message(self, fmt, *args):
        """Clean timestamped server console logs."""
        ts = datetime.now().strftime("%H:%M:%S")
        print(f"[{ts}] {self.address_string()} - {fmt % args}")

    def send_cors_headers(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")

    def send_json(self, payload, status=200):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_cors_headers()
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def read_json_body(self):
        """Safely parse incoming JSON request body."""
        length = int(self.headers.get("Content-Length", 0))
        if length <= 0:
            return {}
        raw = self.rfile.read(length).decode("utf-8", errors="replace")
        try:
            return json.loads(raw)
        except json.JSONDecodeError:
            return None

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_cors_headers()
        self.end_headers()

    # -------------------------------------------------------------------------
    # GET ROUTES
    # -------------------------------------------------------------------------
    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        query = urllib.parse.parse_qs(parsed.query)

        # 1. Serve the PIYUSH HOME Frontend Dashboard
        if path in ("/", "/index.html", "/piyush_home.html"):
            if os.path.exists(FRONTEND_FILE):
                with open(FRONTEND_FILE, "rb") as f:
                    content = f.read()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(content)))
                self.end_headers()
                self.wfile.write(content)
            else:
                msg = (
                    "<h1>PIYUSH HOME Backend Running</h1>"
                    "<p>Place <b>piyush_home.html</b> in the same folder as "
                    "<code>piyush_home_backend.py</code> to load the dashboard.</p>"
                ).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(msg)))
                self.end_headers()
                self.wfile.write(msg)
            return

        # 2. Health Check
        if path == "/api/health":
            self.send_json({"ok": True, "service": "PIYUSH HOME Backend", "port": PORT})
            return

        # 3. Full Unified Dashboard Snapshot (Telemetry + Switches + Settings + Events)
        if path == "/api/state":
            settings = get_all_settings()
            switches = get_all_switches()
            events = get_recent_events(40)
            history = get_telemetry_history(14)
            with state_lock:
                snap = dict(runtime_state)

            self.send_json({
                "ok": True,
                "sensors": {
                    "temp": round(snap["temp"], 1),
                    "humidity": round(snap["humidity"], 1),
                    "mq2": int(snap["mq2"]),
                    "mq135": int(snap["mq135"]),
                    "motion": bool(snap["motion"]),
                    "motionCount": snap["motionCount"],
                    "motionLastTime": snap["motionLastTime"],
                    "tempHistory": [h["temp"] for h in history] if history else [round(snap["temp"], 1)],
                },
                "switches": switches,
                "settings": settings,
                "esp32Online": snap["esp32Online"],
                "lastSync": snap["lastSync"],
                "events": events,
            })
            return

        # 4. ESP32-Compatible Sensor Endpoint
        if path == "/api/sensors":
            switches = get_all_switches()
            with state_lock:
                snap = dict(runtime_state)
            self.send_json({
                "temp": round(snap["temp"], 1),
                "humidity": round(snap["humidity"], 1),
                "mq2": int(snap["mq2"]),
                "mq135": int(snap["mq135"]),
                "motion": bool(snap["motion"]),
                "switches": [sw["state"] for sw in switches],
                "esp32Online": snap["esp32Online"],
            })
            return

        # 5. Switch Control via GET (/api/switch?id=1&state=1)
        if path == "/api/switch":
            try:
                sw_id = int(query.get("id", [0])[0])
                sw_state = int(query.get("state", [-1])[0])
            except ValueError:
                self.send_json({"error": "Invalid id or state"}, status=400)
                return

            if sw_id < 1 or sw_id > 5 or sw_state not in (0, 1):
                self.send_json({"error": "id must be 1-5 and state must be 0 or 1"}, status=400)
                return

            self._handle_switch_change(sw_id, sw_state == 1)
            return

        # 6. Return 20x4 LCD to Live Sensor Mode
        if path == "/api/lcd/live":
            save_setting("lcdMode", "live")
            ok, hw_resp = forward_lcd_to_esp32(live_mode=True)
            log_event_db("20x4 LCD switched to Live Sensor Data mode.", "info")
            self.send_json({"ok": True, "mode": "live", "hardwareSynced": ok, "detail": hw_resp})
            return

        # 7. Telemetry History (JSON)
        if path == "/api/history":
            limit = query.get("limit", [50])[0]
            try:
                rows = get_telemetry_history(int(limit))
            except ValueError:
                rows = get_telemetry_history(50)
            self.send_json({"ok": True, "count": len(rows), "history": rows})
            return

        # 8. Telemetry History Download (CSV)
        if path == "/api/history/csv":
            rows = get_telemetry_history(MAX_TELEMETRY_ROWS)
            buf = io.StringIO()
            writer = csv.writer(buf)
            writer.writerow(["Timestamp", "Temp_C", "Humidity_Pct", "MQ2_PPM", "MQ135_PPM", "Motion_Detected"])
            for r in rows:
                writer.writerow([
                    r["timestamp"], r["temp"], r["humidity"], r["mq2"], r["mq135"],
                    "YES" if r["motion"] else "NO",
                ])
            csv_bytes = buf.getvalue().encode("utf-8")
            self.send_response(200)
            self.send_cors_headers()
            self.send_header("Content-Type", "text/csv; charset=utf-8")
            self.send_header("Content-Disposition", "attachment; filename=piyush_home_telemetry.csv")
            self.send_header("Content-Length", str(len(csv_bytes)))
            self.end_headers()
            self.wfile.write(csv_bytes)
            return

        # 9. Settings & Events
        if path == "/api/settings":
            self.send_json({"ok": True, "settings": get_all_settings()})
            return

        if path == "/api/events":
            self.send_json({"ok": True, "events": get_recent_events(40)})
            return

        self.send_json({"error": f"Route not found: {path}"}, status=404)

    # -------------------------------------------------------------------------
    # POST ROUTES
    # -------------------------------------------------------------------------
    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        body = self.read_json_body()
        if body is None:
            self.send_json({"error": "Invalid JSON body"}, status=400)
            return

        # 1. Toggle Single Switch
        if path == "/api/switch":
            try:
                sw_id = int(body.get("id", 0))
                turn_on = bool(body.get("state"))
            except (TypeError, ValueError):
                self.send_json({"error": "Invalid switch payload"}, status=400)
                return

            if sw_id < 1 or sw_id > 5:
                self.send_json({"error": "Switch id must be 1 to 5"}, status=400)
                return

            self._handle_switch_change(sw_id, turn_on)
            return

        # 2. Master All ON / All OFF
        if path == "/api/switches/all":
            turn_on = bool(body.get("state"))
            hw_ok_all = True
            for sw_id in range(1, 6):
                update_switch_state_db(sw_id, turn_on)
                ok, _ = forward_switch_to_esp32(sw_id, turn_on)
                if not ok:
                    hw_ok_all = False

            log_event_db(f"Master Control: All 5 switches turned {'ON' if turn_on else 'OFF'}", "success" if turn_on else "info")
            self.send_json({
                "ok": True,
                "state": turn_on,
                "switches": get_all_switches(),
                "hardwareSynced": hw_ok_all,
            })
            return

        # 3. Rename Switch (Persisted in SQLite)
        if path == "/api/switch/rename":
            try:
                sw_id = int(body.get("id", 0))
            except (TypeError, ValueError):
                self.send_json({"error": "Invalid switch id"}, status=400)
                return
            new_name = str(body.get("name", "")).strip()
            if sw_id < 1 or sw_id > 5 or not new_name:
                self.send_json({"error": "Provide switch id (1-5) and non-empty name"}, status=400)
                return

            rename_switch_db(sw_id, new_name)
            log_event_db(f"Switch {sw_id} renamed to \"{new_name}\"", "info")
            self.send_json({"ok": True, "switches": get_all_switches()})
            return

        # 4. Push Custom 20x4 LCD Message
        if path == "/api/lcd":
            raw_lines = body.get("lines")
            if not isinstance(raw_lines, list):
                self.send_json({"error": "Expected JSON {\"lines\": [\"L1\",\"L2\",\"L3\",\"L4\"]}"}, status=400)
                return

            lines = [pad_lcd20(raw_lines[i] if i < len(raw_lines) else "") for i in range(4)]
            all_empty = all(line.strip() == "" for line in lines)

            save_setting("lcdCustomLines", json.dumps(lines))
            save_setting("lcdMode", "live" if all_empty else "custom")

            ok, hw_resp = forward_lcd_to_esp32(lines=lines, live_mode=all_empty)
            log_event_db(
                "20x4 LCD cleared -> returned to Live Mode." if all_empty else "Custom 20x4 message sent to LCD display.",
                "success" if not all_empty else "info",
            )
            self.send_json({
                "ok": True,
                "mode": "live" if all_empty else "custom",
                "lines": lines,
                "hardwareSynced": ok,
                "detail": hw_resp,
            })
            return

        # 5. Switch LCD to Live Mode
        if path == "/api/lcd/live":
            save_setting("lcdMode", "live")
            ok, hw_resp = forward_lcd_to_esp32(live_mode=True)
            self.send_json({"ok": True, "mode": "live", "hardwareSynced": ok, "detail": hw_resp})
            return

        # 6. Update Backend & ESP32 Settings
        if path == "/api/settings":
            if "useHardware" in body:
                save_setting("useHardware", "true" if bool(body["useHardware"]) else "false")
            if "esp32BaseUrl" in body and str(body["esp32BaseUrl"]).strip():
                save_setting("esp32BaseUrl", str(body["esp32BaseUrl"]).strip().rstrip("/"))
            if "esp32CamUrl" in body and str(body["esp32CamUrl"]).strip():
                save_setting("esp32CamUrl", str(body["esp32CamUrl"]).strip())
            if "motionArmed" in body:
                save_setting("motionArmed", "true" if bool(body["motionArmed"]) else "false")

            updated = get_all_settings()
            mode_label = f"ESP32 Hardware ({updated['esp32BaseUrl']})" if updated["useHardware"] else "Simulation Mode"
            log_event_db(f"System settings updated -> {mode_label}", "success")
            self.send_json({"ok": True, "settings": updated})
            return

        # 7. Trigger Simulated Test Events (Gas Spike / Poor Air / Motion)
        if path == "/api/simulate":
            sim_type = str(body.get("type", "")).lower()
            with state_lock:
                if sim_type == "gas":
                    runtime_state["gasSpikeTicks"] = 4
                    runtime_state["mq2"] = 515
                    log_event_db("Simulated MQ-2 Gas/Smoke spike (515 PPM)", "danger")
                elif sim_type == "air":
                    runtime_state["airSpikeTicks"] = 4
                    runtime_state["mq135"] = 1140
                    log_event_db("Simulated MQ-135 Poor Air Quality spike (1140 PPM)", "warn")
                elif sim_type == "motion":
                    runtime_state["motion"] = True
                    runtime_state["motionCount"] += 1
                    runtime_state["motionLastTime"] = datetime.now().strftime("%H:%M:%S")
                    runtime_state["motionClearAt"] = time.time() + 6.0
                    log_event_db(
                        f"PIR Motion Sensor triggered at Main Entry (#{runtime_state['motionCount']})",
                        "danger",
                    )
                else:
                    self.send_json({"error": "Unknown simulation type (use gas, air, or motion)"}, status=400)
                    return

            self.send_json({"ok": True, "simulated": sim_type})
            return

        # 8. Forward WiFi Credential Reset to ESP32
        if path == "/api/wifi/reset":
            settings = get_all_settings()
            if not settings["useHardware"]:
                log_event_db("WiFi Reset requested in Simulation Mode (enable ESP32 Hardware first).", "warn")
                self.send_json({
                    "ok": False,
                    "message": "Enable 'Use Real ESP32 Hardware' in ESP32 Setup before resetting device WiFi.",
                }, status=400)
                return

            url = f"{settings['esp32BaseUrl']}/api/wifi/reset"
            ok, hw_resp = esp32_http_request(url, method="POST")
            if ok:
                log_event_db("Sent WiFi Reset command to ESP32. Device rebooting into PIYUSH_HOME_SETUP AP.", "warn")
                self.send_json({
                    "ok": True,
                    "message": "ESP32 WiFi credentials erased! Connect to PIYUSH_HOME_SETUP to reconfigure.",
                    "detail": hw_resp,
                })
            else:
                log_event_db(f"ESP32 WiFi Reset failed (ESP32 unreachable at {settings['esp32BaseUrl']}).", "danger")
                self.send_json({
                    "ok": False,
                    "message": f"Could not reach ESP32 at {settings['esp32BaseUrl']}. Check IP or hold BOOT button 3s.",
                }, status=502)
            return

        self.send_json({"error": f"Route not found: {path}"}, status=404)

    # -------------------------------------------------------------------------
    # DELETE ROUTES
    # -------------------------------------------------------------------------
    def do_DELETE(self):
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/api/events":
            clear_events_db()
            log_event_db("Event log cleared.", "info")
            self.send_json({"ok": True, "events": get_recent_events(40)})
            return
        self.send_json({"error": f"Route not found: {parsed.path}"}, status=404)

    # -------------------------------------------------------------------------
    # SHARED HELPER
    # -------------------------------------------------------------------------
    def _handle_switch_change(self, sw_id, turn_on):
        update_switch_state_db(sw_id, turn_on)
        ok, hw_resp = forward_switch_to_esp32(sw_id, turn_on)
        switches = get_all_switches()
        sw_name = switches[sw_id - 1]["name"]
        log_event_db(
            f"Switch {sw_id} ({sw_name}) turned {'ON' if turn_on else 'OFF'}",
            "success" if turn_on else "info",
        )
        self.send_json({
            "ok": True,
            "id": sw_id,
            "state": turn_on,
            "hardwareSynced": ok,
            "detail": hw_resp,
            "switches": switches,
        })


# =============================================================================
# 7. SERVER BOOTSTRAP
# =============================================================================
def main():
    init_database()
    log_event_db("PIYUSH HOME Backend Server started.", "success")

    # Start background sensor & ESP32 synchronization thread
    worker = threading.Thread(target=telemetry_worker, daemon=True)
    worker.start()

    server = ThreadingHTTPServer((HOST, PORT), PiyushHomeHandler)
    print("=================================================================")
    print("  PIYUSH HOME — Smart IoT Backend Server ONLINE")
    print(f"  Dashboard URL : http://localhost:{PORT}")
    print(f"  SQLite DB     : {DB_PATH}")
    print(f"  Frontend File : {FRONTEND_FILE}")
    print("  Press Ctrl+C to stop the server.")
    print("=================================================================")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[SERVER] Shutting down PIYUSH HOME Backend...")
        server.server_close()


if __name__ == "__main__":
    main()