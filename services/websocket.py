from __future__ import annotations

from typing import Any, Dict

from flask import request
from flask_socketio import SocketIO, emit, join_room, leave_room

socketio = SocketIO(cors_allowed_origins="*", async_mode="threading")


def init_socketio(app) -> SocketIO:
    """Initialize Socket.IO with the Flask app."""
    if app.extensions.get("socketio") is not socketio:
        socketio.init_app(app)
    return socketio


def emit_status_update(event: str, payload: Dict[str, Any]) -> None:
    """Broadcast an update to connected browser clients."""
    socketio.emit(event, payload)


def emit_session_update(event: str, payload: Dict[str, Any], session_id: str) -> None:
    """Broadcast an update only to clients joined to one session room."""
    socketio.emit(event, payload, to=session_id)


@socketio.on("join_session")
def handle_join_session(payload: Dict[str, Any] | None):
    """Join a browser client to a session-specific Socket.IO room."""
    from services.session_manager import session_manager

    session_id = str((payload or {}).get("sessionId", "")).strip().upper()
    snapshot = session_manager.session_snapshot(session_id)
    if not snapshot:
        return {"success": False, "error": "Invalid session ID"}

    join_room(session_id)
    emit("session_joined", {"session": snapshot}, to=request.sid)
    return {"success": True, "session": snapshot}


@socketio.on("leave_session")
def handle_leave_session(payload: Dict[str, Any] | None):
    """Remove a browser client from a session-specific room."""
    session_id = str((payload or {}).get("sessionId", "")).strip().upper()
    if session_id:
        leave_room(session_id)
    return {"success": True}
