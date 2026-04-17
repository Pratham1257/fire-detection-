import sqlite3
from pathlib import Path

DB_PATH = Path("macro_alerts.db")


def get_connection():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


def init_db():
    with get_connection() as conn:
        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS events (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                created_at TEXT NOT NULL,
                device_id TEXT,
                temperature REAL NOT NULL,
                humidity REAL NOT NULL,
                smoke_gas_level REAL NOT NULL,
                flame_detected INTEGER NOT NULL,
                initial_warning TEXT NOT NULL,
                prediction TEXT NOT NULL,
                prediction_confidence REAL NOT NULL DEFAULT 0,
                severity_score INTEGER NOT NULL DEFAULT 0,
                severity_band TEXT NOT NULL DEFAULT 'Safe',
                verification TEXT NOT NULL,
                emergency_level TEXT NOT NULL,
                action TEXT NOT NULL,
                authority TEXT NOT NULL,
                notify_required INTEGER NOT NULL DEFAULT 0
            )
            """
        )

        # Lightweight schema migration for existing local databases.
        existing_cols = {
            row["name"] for row in conn.execute("PRAGMA table_info(events)").fetchall()
        }
        if "prediction_confidence" not in existing_cols:
            conn.execute(
                "ALTER TABLE events ADD COLUMN prediction_confidence REAL NOT NULL DEFAULT 0"
            )
        if "severity_score" not in existing_cols:
            conn.execute(
                "ALTER TABLE events ADD COLUMN severity_score INTEGER NOT NULL DEFAULT 0"
            )
        if "severity_band" not in existing_cols:
            conn.execute(
                "ALTER TABLE events ADD COLUMN severity_band TEXT NOT NULL DEFAULT 'Safe'"
            )

        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS notifications (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                event_id INTEGER NOT NULL,
                authority TEXT NOT NULL,
                status TEXT NOT NULL,
                created_at TEXT NOT NULL,
                FOREIGN KEY(event_id) REFERENCES events(id)
            )
            """
        )


def insert_event(event):
    with get_connection() as conn:
        cursor = conn.execute(
            """
            INSERT INTO events (
                created_at,
                device_id,
                temperature,
                humidity,
                smoke_gas_level,
                flame_detected,
                initial_warning,
                prediction,
                prediction_confidence,
                severity_score,
                severity_band,
                verification,
                emergency_level,
                action,
                authority,
                notify_required
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                event["created_at"],
                event.get("device_id"),
                event["temperature"],
                event["humidity"],
                event["smoke_gas_level"],
                event["flame_detected"],
                event["initial_warning"],
                event["prediction"],
                event["prediction_confidence"],
                event["severity_score"],
                event["severity_band"],
                event["verification"],
                event["emergency_level"],
                event["action"],
                event["authority"],
                int(event["notify_required"]),
            ),
        )
        return cursor.lastrowid


def insert_notifications(event_id, authorities, created_at):
    if not authorities:
        return

    with get_connection() as conn:
        conn.executemany(
            """
            INSERT INTO notifications (event_id, authority, status, created_at)
            VALUES (?, ?, ?, ?)
            """,
            [(event_id, authority, "queued", created_at) for authority in authorities],
        )


def find_recent_duplicate_event(
    device_id,
    prediction,
    smoke_gas_level,
    cutoff_created_at,
    smoke_bucket_size=100,
):
    if not device_id:
        return None

    bucket_min = int(smoke_gas_level // smoke_bucket_size) * smoke_bucket_size
    bucket_max = bucket_min + smoke_bucket_size

    with get_connection() as conn:
        row = conn.execute(
            """
            SELECT id, created_at, device_id, temperature, humidity, smoke_gas_level,
                     flame_detected, initial_warning, prediction, prediction_confidence, severity_score,
                     severity_band, verification,
                   emergency_level, action, authority, notify_required
            FROM events
            WHERE device_id = ?
              AND prediction = ?
              AND smoke_gas_level >= ?
              AND smoke_gas_level < ?
              AND created_at >= ?
            ORDER BY id DESC
            LIMIT 1
            """,
            (device_id, prediction, bucket_min, bucket_max, cutoff_created_at),
        ).fetchone()

    return dict(row) if row else None


def get_recent_events(limit=200):
    with get_connection() as conn:
        rows = conn.execute(
            """
            SELECT id, created_at, device_id, temperature, humidity, smoke_gas_level,
                                         flame_detected, initial_warning, prediction, prediction_confidence, severity_score,
                                         severity_band, verification,
                   emergency_level, action, authority, notify_required
            FROM events
            ORDER BY id DESC
            LIMIT ?
            """,
            (limit,),
        ).fetchall()
    return [dict(row) for row in rows]


def get_dashboard_stats():
    with get_connection() as conn:
        total_events = conn.execute("SELECT COUNT(*) AS c FROM events").fetchone()["c"]
        false_alarms = conn.execute(
            "SELECT COUNT(*) AS c FROM events WHERE prediction = 'False Alarm'"
        ).fetchone()["c"]
        real_alerts = conn.execute(
            "SELECT COUNT(*) AS c FROM events WHERE prediction != 'False Alarm'"
        ).fetchone()["c"]
        critical_events = conn.execute(
            "SELECT COUNT(*) AS c FROM events WHERE prediction = 'Fire Emergency'"
        ).fetchone()["c"]

    return {
        "total_events": total_events,
        "false_alarms": false_alarms,
        "real_alerts": real_alerts,
        "critical_events": critical_events,
    }
