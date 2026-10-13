# Plan Scheduler Robot: Backend

FastAPI + SQLite + LLM tool calling (free Groq API by default; any OpenAI-compatible provider works). The ESP32 robot and the web dashboard both talk to this server.

## Setup

```bash
cd assistant-backend
python -m venv venv
source venv/bin/activate          # Windows: venv\Scripts\activate
pip install -r requirements.txt
cp .env.example .env              # then edit: LLM_API_KEY (free key from console.groq.com/keys), timezone
uvicorn main:app --host 0.0.0.0 --port 8000 --reload
```

Open http://localhost:8000 for the dashboard, or http://localhost:8000/docs to try every endpoint.
`--host 0.0.0.0` lets the ESP32 reach the server over your WiFi
(use your PC's local IP, e.g. `http://192.168.1.20:8000`).

## Test without the robot

```bash
# Add plans by talking to the assistant
curl -X POST localhost:8000/api/command -H "Content-Type: application/json" \
  -d '{"text": "Add a team meeting tomorrow at 9 AM and a client call at 1 PM"}'

curl -X POST localhost:8000/api/command -H "Content-Type: application/json" \
  -d '{"text": "Move the 1 o clock meeting to 3"}'

curl -X POST localhost:8000/api/command -H "Content-Type: application/json" \
  -d '{"text": "Cancel the 9 AM meeting tomorrow"}'

curl -X POST localhost:8000/api/command -H "Content-Type: application/json" \
  -d '{"text": "Set an alarm for 5 AM"}'

# Morning flow: greeting, then "yes"
curl -X POST localhost:8000/api/robot/morning
curl -X POST localhost:8000/api/command -H "Content-Type: application/json" \
  -d '{"text": "yes", "session_id": "robot"}'
```

## Endpoints

| Method | Path | Purpose |
|---|---|---|
| POST | `/api/command` | Text in, spoken reply out (the main AI endpoint) |
| GET/POST | `/api/events` | List / add plans (dashboard) |
| PATCH/DELETE | `/api/events/{id}` | Edit / delete a plan |
| GET/POST | `/api/alarms` | List / add alarms |
| DELETE | `/api/alarms/{id}` | Delete an alarm |
| GET | `/api/robot/sync` | Server time + enabled alarms for the ESP32 |
| POST | `/api/robot/alarms/{id}/fired` | ESP32 reports an alarm rang |
| POST | `/api/robot/morning` | Greeting after the alarm is switched off |
| GET | `/api/robot/plans-summary` | Fast spoken list of today's plans |
| POST | `/api/robot/end-session` | Clear conversation memory |
| WS | `/ws` | Pushes `{"type":"update"}` so dashboards refresh live |

## How the pieces fit

- `db.py` stores events and alarms in SQLite.
- `assistant.py` gives the LLM tools (`add_event`, `cancel_event`, `move_event`, `list_events`, `set_alarm`, `cancel_alarm`, `list_alarms`). If a request matches several events ("cancel the meeting" when there are two), nothing is changed and the assistant asks which one you mean.
- `static/index.html` is the dashboard, served at `/`. It shows a week strip and the day's plans, lets you change a plan's time or delete it, manages alarms, and has a box to type commands to the assistant. It refreshes itself over the WebSocket.
- `main.py` exposes everything over HTTP and broadcasts a WebSocket message after each change.

## Next steps

1. Speech-to-text and text-to-speech endpoints, so the ESP32 can send audio instead of text
2. ESP32 firmware: WiFi, NTP, alarm ringing, OLED faces, LDR sleep/wake
