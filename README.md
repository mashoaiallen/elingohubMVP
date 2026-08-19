# eLingo Hub MVP

This directory contains the local prototype backend for the eLingo Hub MVP.

## What is implemented

- Flask application shell with a modular layout
- Session start, status, and end APIs
- Multipart audio chunk upload storage
- Live dashboard updates over Socket.IO
- Structured logging for session and upload events

## Run locally

```bash
python -m venv .venv
source .venv/bin/activate  # On Windows use .venv\Scripts\activate
pip install -r requirements.txt
python app.py
```

Then open http://127.0.0.1:5000/ in your browser.

## API highlights

- POST /api/session/start
- GET /api/session/<session_id>
- POST /api/session/end
- POST /api/session/audio

## Tests

```bash
python -m pytest -q
```
