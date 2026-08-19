from pathlib import Path

from flask import Blueprint, current_app, jsonify, request, send_from_directory

from services.chunk_storage import ChunkStorage
from services.session_manager import session_manager
from services.websocket import emit_session_update, emit_status_update

session_bp = Blueprint("session", __name__, url_prefix="/api/session")
session_storage = ChunkStorage(session_manager=session_manager)


@session_bp.post("/start")
def start_session():
    """Create a new session and return a session identifier."""
    session_id = session_manager.create_session()
    session_storage.prepare_session(session_id)
    current_app.logger.info("Session created: %s", session_id)
    emit_status_update("session_created", {"sessionId": session_id})
    return jsonify({"sessionId": session_id}), 200


@session_bp.get("/current")
def get_current_session():
    """Return the latest session state for dashboard polling."""
    return jsonify(session_manager.latest_snapshot()), 200


@session_bp.get("/<session_id>")
def get_session(session_id: str):
    """Return the current state of a session."""
    session = session_manager.get_session(session_id)
    if not session:
        return jsonify({"error": "Invalid session ID"}), 404

    return jsonify(session_manager.session_snapshot(session_id)), 200


@session_bp.get("/chunk/<session_id>/<chunk_filename>")
def serve_chunk(session_id: str, chunk_filename: str):
    """Serve a saved chunk from the uploads directory."""
    upload_root = Path(current_app.config["UPLOAD_FOLDER"])
    session_dir = upload_root / session_id
    if not session_dir.exists() or not session_dir.is_dir():
        return jsonify({"error": "Chunk not found"}), 404

    chunk_path = session_dir / chunk_filename
    if not chunk_path.exists() or not chunk_path.is_file():
        return jsonify({"error": "Chunk not found"}), 404

    current_app.logger.info("Serving chunk %s for session %s", chunk_filename, session_id)
    return send_from_directory(str(session_dir), chunk_filename, mimetype="audio/wav")


@session_bp.post("/end")
def end_session():
    """Close an active session."""
    payload = request.get_json(silent=True) or {}
    session_id = payload.get("sessionId")

    if not session_id:
        return jsonify({"error": "sessionId is required"}), 400

    if not session_manager.end_session(session_id):
        current_app.logger.warning("Attempted to end invalid session: %s", session_id)
        return jsonify({"error": "Invalid session ID"}), 404

    current_app.logger.info("Session ended: %s", session_id)
    session_storage.write_metadata(session_id)
    emit_status_update("session_ended", {"sessionId": session_id})
    emit_session_update(
        "session_ended",
        {
            "sessionId": session_id,
            "status": "ended",
        },
        session_id,
    )
    return jsonify({"message": "Session ended"}), 200
