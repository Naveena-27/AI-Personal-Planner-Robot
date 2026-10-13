"""Turns natural language into schedule changes using LLM tool calling.

Uses the OpenAI-compatible chat API, so it works with Groq (free), Gemini's
free tier, Ollama (local) or any similar provider: only .env changes."""
import json
import os
from datetime import datetime, timedelta
from zoneinfo import ZoneInfo

from openai import OpenAI

import db

MODEL = os.getenv("MODEL", "llama-3.3-70b-versatile")
BASE_URL = os.getenv("LLM_BASE_URL", "https://api.groq.com/openai/v1")
USER_NAME = os.getenv("USER_NAME", "Naveena")
TZ = ZoneInfo(os.getenv("TIMEZONE", "UTC"))

_client = None
_sessions = {}  # session_id -> list of {"role", "content"} (text only)
MAX_HISTORY = 12


def client():
    global _client
    if _client is None:
        _client = OpenAI(base_url=BASE_URL, api_key=os.getenv("LLM_API_KEY", "none"))
    return _client


def now_local():
    return datetime.now(TZ)


# ------------------------------------------------------------- speech text
def speak_time(t):
    h, m = map(int, t.split(":"))
    suffix = "AM" if h < 12 else "PM"
    h12 = h % 12 or 12
    return f"{h12} {suffix}" if m == 0 else f"{h12}:{m:02d} {suffix}"


def plans_summary(date):
    """Deterministic spoken summary (no LLM, so it is fast)."""
    events = db.list_events(date)
    if not events:
        return "You have no plans today."
    parts = [f"At {speak_time(e['start_time'])}, {e['title']}." for e in events]
    n = len(events)
    return f"You have {n} plan{'s' if n > 1 else ''} today. " + " ".join(parts)


def greeting():
    hour = now_local().hour
    part = "morning" if hour < 12 else "afternoon" if hour < 18 else "evening"
    return f"Good {part} {USER_NAME}, do you want to know today's plans?"


# ------------------------------------------------------------------- tools
DATE = {"type": "string", "description": "Date as YYYY-MM-DD"}
TIME = {"type": "string", "description": "24-hour time as HH:MM"}

TOOLS = [
    {
        "name": "list_events",
        "description": "List plans/events. Use for questions like 'what do I have today/tomorrow/this week'.",
        "input_schema": {
            "type": "object",
            "properties": {
                "date_from": DATE,
                "date_to": {**DATE, "description": "Optional end date for a range (inclusive)"},
            },
            "required": ["date_from"],
        },
    },
    {
        "name": "add_event",
        "description": "Schedule a new event.",
        "input_schema": {
            "type": "object",
            "properties": {
                "title": {"type": "string"},
                "date": DATE,
                "start_time": TIME,
                "notes": {"type": "string"},
            },
            "required": ["title", "date", "start_time"],
        },
    },
    {
        "name": "cancel_event",
        "description": (
            "Cancel an event. Identify it by date plus start time and/or part of the "
            "title. If several events match, nothing is deleted and the matches are "
            "returned so you can ask the user which one."
        ),
        "input_schema": {
            "type": "object",
            "properties": {
                "date": DATE,
                "time": {**TIME, "description": "Current start time of the event"},
                "title_contains": {"type": "string"},
            },
            "required": ["date"],
        },
    },
    {
        "name": "move_event",
        "description": (
            "Reschedule an event to a new time and/or date. Identify the event with "
            "its current date plus time and/or title. If several match, nothing changes "
            "and the matches are returned so you can ask the user which one."
        ),
        "input_schema": {
            "type": "object",
            "properties": {
                "date": {**DATE, "description": "Current date of the event"},
                "time": {**TIME, "description": "Current start time of the event"},
                "title_contains": {"type": "string"},
                "new_date": DATE,
                "new_time": TIME,
            },
            "required": ["date"],
        },
    },
    {
        "name": "set_alarm",
        "description": "Create an alarm on the robot.",
        "input_schema": {
            "type": "object",
            "properties": {
                "time": TIME,
                "label": {"type": "string"},
                "repeat": {"type": "string", "enum": ["once", "daily", "weekdays"]},
            },
            "required": ["time"],
        },
    },
    {
        "name": "cancel_alarm",
        "description": "Delete the alarm(s) set for a given time.",
        "input_schema": {
            "type": "object",
            "properties": {"time": TIME},
            "required": ["time"],
        },
    },
    {
        "name": "list_alarms",
        "description": "List all alarms.",
        "input_schema": {"type": "object", "properties": {}},
    },
]


def _brief(e):
    return {
        "id": e["id"],
        "title": e["title"],
        "date": e["date"],
        "time": e["start_time"],
    }


def _resolve(args):
    matches = db.find_events(args["date"], args.get("time"), args.get("title_contains"))
    if not matches:
        return None, {"error": "No matching event found on that date."}
    if len(matches) > 1:
        return None, {
            "ambiguous": True,
            "message": "Several events match. Ask the user which one.",
            "matches": [_brief(e) for e in matches],
        }
    return matches[0], None


def run_tool(name, args):
    if name == "list_events":
        events = db.list_events(args["date_from"], args.get("date_to"))
        return {"events": [_brief(e) for e in events]}

    if name == "add_event":
        event, conflicts = db.add_event(
            args["title"], args["date"], args["start_time"], args.get("notes", "")
        )
        out = {"added": _brief(event)}
        if conflicts:
            out["warning_conflicts_with"] = [_brief(e) for e in conflicts]
        return out

    if name == "cancel_event":
        event, err = _resolve(args)
        if err:
            return err
        db.delete_event(event["id"])
        return {"cancelled": _brief(event)}

    if name == "move_event":
        if not args.get("new_time") and not args.get("new_date"):
            return {"error": "Need new_time and/or new_date."}
        event, err = _resolve(args)
        if err:
            return err
        updated = db.update_event(
            event["id"],
            date=args.get("new_date"),
            start_time=args.get("new_time"),
        )
        out = {"moved_from": _brief(event), "moved_to": _brief(updated)}
        clash = [
            e
            for e in db.find_events(updated["date"], updated["start_time"])
            if e["id"] != updated["id"]
        ]
        if clash:
            out["warning_conflicts_with"] = [_brief(e) for e in clash]
        return out

    if name == "set_alarm":
        alarm = db.add_alarm(args["time"], args.get("label", ""), args.get("repeat", "once"))
        return {"alarm_set": alarm}

    if name == "cancel_alarm":
        n = db.delete_alarms_at(args["time"])
        return {"deleted": n} if n else {"error": "No alarm at that time."}

    if name == "list_alarms":
        return {"alarms": db.list_alarms()}

    return {"error": f"Unknown tool {name}"}


# ------------------------------------------------------------ conversation
def system_prompt():
    now = now_local()
    tomorrow = (now + timedelta(days=1)).strftime("%Y-%m-%d")
    return f"""You are the voice of a small desk robot that is {USER_NAME}'s personal plan scheduler.
Current local date/time: {now.strftime('%A, %Y-%m-%d %H:%M')} (tomorrow is {tomorrow}).

Rules:
- Your replies are spoken aloud. Use short, natural sentences. No markdown, lists, emojis or symbols.
- Say times the way people speak, like "3 PM" or "1:30 PM".
- Always call tools to read or change plans. Never guess what is on the schedule.
- Convert relative dates ("tomorrow", "next Monday") to YYYY-MM-DD yourself.
- Times with no AM/PM: pick the most sensible one for a normal day (a "1 o'clock meeting" is 13:00; "5 o'clock alarm" is ambiguous, so ask if unsure).
- If a tool reports several matches, ask which one the user means. If something is missing, ask one short question.
- After changing something, confirm what you did in one sentence. Mention any conflict warning.
- If the user says yes to hearing today's plans, call list_events for today and read them out.
- If the user is clearly finished (for example "thanks" or "that's all"), say a brief goodbye and put the marker [END] at the very end of your reply."""


def _trim(history):
    del history[:-MAX_HISTORY]
    while history and history[0]["role"] != "user":
        history.pop(0)


def seed_session(session_id, assistant_text):
    """Pre-load the assistant's opening line (used by the morning flow) so a
    later 'yes' is understood in context."""
    _sessions[session_id] = [
        {"role": "user", "content": "(alarm was switched off)"},
        {"role": "assistant", "content": assistant_text},
    ]


def end_session(session_id):
    _sessions.pop(session_id, None)


# OpenAI-style version of the tool list
OPENAI_TOOLS = [
    {
        "type": "function",
        "function": {
            "name": t["name"],
            "description": t["description"],
            "parameters": t["input_schema"],
        },
    }
    for t in TOOLS
]


def run_command(text, session_id="default"):
    """Returns (reply_text, changed, end_session).
    changed: the database was modified. end_session: the user is done talking."""
    history = _sessions.setdefault(session_id, [])
    messages = (
        [{"role": "system", "content": system_prompt()}]
        + history
        + [{"role": "user", "content": text}]
    )
    changed = False
    msg = None

    for _ in range(6):  # safety cap on tool loops
        resp = client().chat.completions.create(
            model=MODEL,
            messages=messages,
            tools=OPENAI_TOOLS,
            tool_choice="auto",
            max_tokens=500,
            temperature=0.2,
        )
        msg = resp.choices[0].message
        if not msg.tool_calls:
            break

        messages.append(
            {
                "role": "assistant",
                "content": msg.content or "",
                "tool_calls": [
                    {
                        "id": tc.id,
                        "type": "function",
                        "function": {
                            "name": tc.function.name,
                            "arguments": tc.function.arguments or "{}",
                        },
                    }
                    for tc in msg.tool_calls
                ],
            }
        )
        for tc in msg.tool_calls:
            try:
                args = json.loads(tc.function.arguments or "{}") or {}
                out = run_tool(tc.function.name, args)
            except Exception as exc:  # bad JSON, missing field, etc.: let the model retry
                out = {"error": f"Tool call failed: {exc}"}
            if (
                tc.function.name not in ("list_events", "list_alarms")
                and "error" not in out
                and not out.get("ambiguous")
            ):
                changed = True
            messages.append(
                {"role": "tool", "tool_call_id": tc.id, "content": json.dumps(out)}
            )

    reply = ((msg.content if msg else "") or "").strip()
    ended = "[END]" in reply
    reply = reply.replace("[END]", "").strip()
    if not reply:
        reply = "Sorry, I didn't catch that. Could you say it again?"

    history.append({"role": "user", "content": text})
    history.append({"role": "assistant", "content": reply})
    _trim(history)
    if ended:
        end_session(session_id)
    return reply, changed, ended
