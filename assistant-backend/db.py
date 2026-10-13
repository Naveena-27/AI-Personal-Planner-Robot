"""SQLite storage for events (plans) and alarms.

Conventions used everywhere in the project:
  date  -> "YYYY-MM-DD"
  time  -> "HH:MM" (24-hour)
"""
import os
import sqlite3

DB_PATH = os.getenv("DB_PATH", "assistant.db")


def _conn():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


def init_db():
    with _conn() as c:
        c.executescript(
            """
            CREATE TABLE IF NOT EXISTS events (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                title TEXT NOT NULL,
                date TEXT NOT NULL,
                start_time TEXT NOT NULL,
                notes TEXT DEFAULT ''
            );
            CREATE TABLE IF NOT EXISTS alarms (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                time TEXT NOT NULL,
                label TEXT DEFAULT '',
                repeat TEXT NOT NULL DEFAULT 'once',  -- once | daily | weekdays
                enabled INTEGER NOT NULL DEFAULT 1
            );
            """
        )


def _all(cur):
    return [dict(r) for r in cur.fetchall()]


# ---------------------------------------------------------------- events
def list_events(date_from, date_to=None):
    date_to = date_to or date_from
    with _conn() as c:
        cur = c.execute(
            "SELECT * FROM events WHERE date BETWEEN ? AND ? "
            "ORDER BY date, start_time",
            (date_from, date_to),
        )
        return _all(cur)


def add_event(title, date, start_time, notes=""):
    """Adds an event. Returns (event, conflicts) where conflicts are events
    already scheduled at the same date and time."""
    with _conn() as c:
        conflicts = _all(
            c.execute(
                "SELECT * FROM events WHERE date=? AND start_time=?",
                (date, start_time),
            )
        )
        cur = c.execute(
            "INSERT INTO events (title, date, start_time, notes) VALUES (?,?,?,?)",
            (title, date, start_time, notes),
        )
        event = dict(
            c.execute("SELECT * FROM events WHERE id=?", (cur.lastrowid,)).fetchone()
        )
    return event, conflicts


def find_events(date, time=None, title_contains=None):
    query = "SELECT * FROM events WHERE date=?"
    params = [date]
    if time:
        query += " AND start_time=?"
        params.append(time)
    if title_contains:
        query += " AND LOWER(title) LIKE ?"
        params.append(f"%{title_contains.lower()}%")
    with _conn() as c:
        return _all(c.execute(query + " ORDER BY start_time", params))


def get_event(event_id):
    with _conn() as c:
        row = c.execute("SELECT * FROM events WHERE id=?", (event_id,)).fetchone()
        return dict(row) if row else None


def delete_event(event_id):
    with _conn() as c:
        return c.execute("DELETE FROM events WHERE id=?", (event_id,)).rowcount > 0


def update_event(event_id, **fields):
    allowed = {"title", "date", "start_time", "notes"}
    fields = {k: v for k, v in fields.items() if k in allowed and v is not None}
    if not fields:
        return get_event(event_id)
    sets = ", ".join(f"{k}=?" for k in fields)
    with _conn() as c:
        c.execute(f"UPDATE events SET {sets} WHERE id=?", (*fields.values(), event_id))
    return get_event(event_id)


# ---------------------------------------------------------------- alarms
def list_alarms(enabled_only=False):
    query = "SELECT * FROM alarms"
    if enabled_only:
        query += " WHERE enabled=1"
    with _conn() as c:
        return _all(c.execute(query + " ORDER BY time"))


def add_alarm(time, label="", repeat="once"):
    with _conn() as c:
        cur = c.execute(
            "INSERT INTO alarms (time, label, repeat) VALUES (?,?,?)",
            (time, label, repeat),
        )
        return dict(
            c.execute("SELECT * FROM alarms WHERE id=?", (cur.lastrowid,)).fetchone()
        )


def delete_alarms_at(time):
    with _conn() as c:
        return c.execute("DELETE FROM alarms WHERE time=?", (time,)).rowcount


def delete_alarm(alarm_id):
    with _conn() as c:
        return c.execute("DELETE FROM alarms WHERE id=?", (alarm_id,)).rowcount > 0


def mark_alarm_fired(alarm_id):
    """One-time alarms switch off after ringing; repeating ones stay on."""
    with _conn() as c:
        c.execute(
            "UPDATE alarms SET enabled=0 WHERE id=? AND repeat='once'", (alarm_id,)
        )
