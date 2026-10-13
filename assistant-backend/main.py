"""Plan-scheduler robot backend.

Run:  uvicorn main:app --host 0.0.0.0 --port 8000 --reload
Docs: http://localhost:8000/docs
"""
from dotenv import load_dotenv

load_dotenv()  # must run before db/assistant read their env vars

from pathlib import Path
from typing import Optional

from fastapi import FastAPI, HTTPException, Request, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse, Response
from pydantic import BaseModel
from starlette.concurrency import run_in_threadpool

import assistant
import db
import voice

app = FastAPI(title="Plan Scheduler Robot")
app.add_middleware(CORSMiddleware, allow_origins=["*"], allow_methods=["*"], allow_headers=["*"])
db.init_db()


# ------------------------------------------------------- live dashboard (WS)
class Hub:
    def __init__(self):
        self.clients = set()

    async def connect(self, ws: WebSocket):
        await ws.accept()
        self.clients.add(ws)

    def disconnect(self, ws: WebSocket):
        self.clients.discard(ws)

    async def notify(self):
        """Tell every open dashboard to refetch its data."""
        for ws in list(self.clients):
            try:
                await ws.send_json({"type": "update"})
            except Exception:
                self.clients.discard(ws)


hub = Hub()


@app.websocket("/ws")
async def websocket_endpoint(ws: WebSocket):
    await hub.connect(ws)
    try:
        while True:
            await ws.receive_text()  # keep-alive; content ignored
    except WebSocketDisconnect:
        hub.disconnect(ws)


# ----------------------------------------------------------------- schemas
class EventIn(BaseModel):
    title: str
    date: str  # YYYY-MM-DD
    start_time: str  # HH:MM
    notes: str = ""


class EventPatch(BaseModel):
    title: Optional[str] = None
    date: Optional[str] = None
    start_time: Optional[str] = None
    notes: Optional[str] = None


class AlarmIn(BaseModel):
    time: str  # HH:MM
    label: str = ""
    repeat: str = "once"  # once | daily | weekdays


class CommandIn(BaseModel):
    text: str
    session_id: str = "default"


# ------------------------------------------------------------ events (REST)
@app.get("/api/events")
def get_events(date_from: Optional[str] = None, date_to: Optional[str] = None):
    date_from = date_from or assistant.now_local().strftime("%Y-%m-%d")
    return db.list_events(date_from, date_to)


@app.post("/api/events")
async def create_event(e: EventIn):
    event, conflicts = db.add_event(e.title, e.date, e.start_time, e.notes)
    await hub.notify()
    return {"event": event, "conflicts": conflicts}


@app.patch("/api/events/{event_id}")
async def patch_event(event_id: int, p: EventPatch):
    if not db.get_event(event_id):
        raise HTTPException(404, "Event not found")
    event = db.update_event(event_id, **p.model_dump())
    await hub.notify()
    return event


@app.delete("/api/events/{event_id}")
async def remove_event(event_id: int):
    if not db.delete_event(event_id):
        raise HTTPException(404, "Event not found")
    await hub.notify()
    return {"ok": True}


# ------------------------------------------------------------ alarms (REST)
@app.get("/api/alarms")
def get_alarms():
    return db.list_alarms()


@app.post("/api/alarms")
async def create_alarm(a: AlarmIn):
    alarm = db.add_alarm(a.time, a.label, a.repeat)
    await hub.notify()
    return alarm


@app.delete("/api/alarms/{alarm_id}")
async def remove_alarm(alarm_id: int):
    if not db.delete_alarm(alarm_id):
        raise HTTPException(404, "Alarm not found")
    await hub.notify()
    return {"ok": True}


# -------------------------------------------------- the AI command endpoint
@app.post("/api/command")
async def command(c: CommandIn):
    """Text in, spoken reply out. The robot sends the transcribed speech here;
    for now you can test it with curl or the /docs page."""
    try:
        reply, changed, ended = await run_in_threadpool(
            assistant.run_command, c.text, c.session_id
        )
    except Exception as exc:  # e.g. missing API key, network error
        raise HTTPException(502, f"Assistant error: {exc}")
    if changed:
        await hub.notify()
    return {"reply": reply, "changed": changed, "end_session": ended}


# ---------------------------------------------------------- robot endpoints
@app.get("/api/robot/sync")
def robot_sync():
    """ESP32 calls this on boot and every few minutes: it gets the current time
    and the enabled alarms, then rings them itself (works even if WiFi drops)."""
    now = assistant.now_local()
    return {
        "server_time": now.isoformat(timespec="seconds"),
        "timezone": str(assistant.TZ),
        "alarms": db.list_alarms(enabled_only=True),
    }


@app.post("/api/robot/alarms/{alarm_id}/fired")
async def alarm_fired(alarm_id: int):
    db.mark_alarm_fired(alarm_id)
    await hub.notify()
    return {"ok": True}


@app.post("/api/robot/morning")
async def robot_morning(session_id: str = "robot"):
    """Call after the alarm is switched off. Returns the greeting (text + audio)
    and opens a session so the next 'yes' is understood in context."""
    text = assistant.greeting()
    assistant.seed_session(session_id, text)
    return {"reply": text, "audio_id": await voice.speak(text)}


@app.get("/api/robot/plans-summary")
async def plans_summary():
    """Fast, LLM-free reading of today's plans (text + audio)."""
    today = assistant.now_local().strftime("%Y-%m-%d")
    text = assistant.plans_summary(today)
    return {"reply": text, "audio_id": await voice.speak(text)}


@app.get("/api/robot/good-night")
async def good_night():
    text = f"Good night {assistant.USER_NAME}."
    return {"reply": text, "audio_id": await voice.speak(text)}


@app.post("/api/robot/voice")
async def robot_voice(request: Request, session_id: str = "robot"):
    """The robot streams raw 16 kHz 16-bit mono PCM here. We transcribe it, let the
    assistant act on it, and return the reply as text plus an audio_id to download."""
    pcm = await request.body()
    if len(pcm) < 8000 or len(pcm) > 16000 * 2 * 30:  # under 0.25 s or over 30 s
        return {"no_speech": True, "heard": "", "reply": "", "audio_id": None,
                "end_session": False, "changed": False}
    try:
        heard = await run_in_threadpool(voice.transcribe, pcm)
    except Exception as exc:
        raise HTTPException(502, f"Speech-to-text error: {exc}")
    print(f"Heard: {heard!r}")
    if not heard:
        return {"no_speech": True, "heard": "", "reply": "", "audio_id": None,
                "end_session": False, "changed": False}
    try:
        reply, changed, ended = await run_in_threadpool(assistant.run_command, heard, session_id)
    except Exception as exc:
        raise HTTPException(502, f"Assistant error: {exc}")
    if changed:
        await hub.notify()
    return {"no_speech": False, "heard": heard, "reply": reply, "changed": changed,
            "end_session": ended, "audio_id": await voice.speak(reply)}


@app.get("/api/robot/audio/{audio_id}")
def robot_audio(audio_id: str):
    pcm = voice.get_audio(audio_id)
    if pcm is None:
        raise HTTPException(404, "Audio not found")
    return Response(content=pcm, media_type="application/octet-stream")


@app.post("/api/robot/end-session")
def end_session(session_id: str = "robot"):
    assistant.end_session(session_id)
    return {"ok": True}


# --------------------------------------------------------------- dashboard
@app.get("/", include_in_schema=False)
def dashboard():
    return FileResponse(Path(__file__).parent / "static" / "index.html")
