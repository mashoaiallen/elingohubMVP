from datetime import datetime, timezone
from typing import Any, Dict

from flask import Blueprint, current_app, jsonify, request

from services.chunk_storage import ChunkStorage
from services.session_manager import session_manager
from services.transcription import transcribe_chunk
from services.websocket import emit_session_update, emit_status_update, socketio

audio_bp = Blueprint("audio", __name__, url_prefix="/api/session")
chunk_storage = ChunkStorage(session_manager=session_manager)


def _utc_timestamp() -> str:
    return datetime.now(timezone.utc).isoformat()


def _transcribe_and_broadcast(
    app,
    session_id: str,
    chunk_number: int,
    saved_path: str,
    metadata: Dict[str, Any],
) -> None:
    """Transcribe a saved chunk without risking the upload path."""
    with app.app_context():
        timestamp = _utc_timestamp()
        transcript = ""

        try:
            transcript = transcribe_chunk(saved_path)
            chunk_storage.append_transcript(
                session_id,
                chunk_number,
                transcript,
                timestamp,
            )

            current_app.logger.info(
                "Transcribed chunk %s for session %s; has text: %s",
                chunk_number,
                session_id,
                bool(transcript),
            )
            emit_status_update(
                "chunk_transcribed",
                {
                    "sessionId": session_id,
                    "chunkNumber": chunk_number,
                    "hasTranscript": bool(transcript),
                    "timestamp": timestamp,
                    **metadata,
                },
            )

            payload = {
                "sessionId": session_id,
                "chunkNumber": chunk_number,
                "transcript": transcript,
                "hasTranscript": bool(transcript),
                "timestamp": timestamp,
                **metadata,
            }
            emit_session_update("transcript_update", payload, session_id)
            current_app.logger.info(
                "Broadcast transcription result for chunk %s in session %s",
                chunk_number,
                session_id,
            )
            emit_status_update(
                "transcription_broadcast",
                {
                    "sessionId": session_id,
                    "chunkNumber": chunk_number,
                    "hasTranscript": bool(transcript),
                    "timestamp": timestamp,
                    **metadata,
                },
            )
        except Exception as exc:
            current_app.logger.exception(
                "Transcription task failed for chunk %s in session %s",
                chunk_number,
                session_id,
            )
            chunk_storage.append_transcript(session_id, chunk_number, "", timestamp)
            emit_status_update(
                "chunk_transcribed",
                {
                    "sessionId": session_id,
                    "chunkNumber": chunk_number,
                    "hasTranscript": False,
                    "timestamp": timestamp,
                    "error": str(exc),
                    **metadata,
                },
            )


@audio_bp.post("/audio")
def upload_audio_chunk():
    """Handle multipart audio chunk uploads."""
    session_id = request.form.get("sessionId")
    chunk_number = request.form.get("chunkNumber")

    if not session_id or not chunk_number:
        current_app.logger.warning("Missing chunk metadata for audio upload")
        return jsonify({"error": "sessionId and chunkNumber are required"}), 400

    try:
        chunk_number_int = int(chunk_number)
    except ValueError:
        current_app.logger.warning("Invalid chunk number for audio upload: %s", chunk_number)
        return jsonify({"error": "chunkNumber must be an integer"}), 400

    audio_file = request.files.get("audioFile")
    if not audio_file or not audio_file.filename:
        current_app.logger.warning("Audio upload missing file for session %s", session_id)
        return jsonify({"error": "audioFile is required"}), 400

    try:
        saved_path, metadata = chunk_storage.store_chunk(
            session_id, chunk_number, audio_file
        )
    except ValueError as exc:
        error_message = str(exc)
        status_code = 404 if error_message == "Invalid session ID" else 400
        current_app.logger.warning("Audio upload failed: %s", error_message)
        return jsonify({"error": error_message}), status_code

    current_app.logger.info("Chunk uploaded for session %s: %s", session_id, saved_path)
    emit_status_update(
        "chunk_uploaded",
        {
            "sessionId": session_id,
            "chunkNumber": chunk_number_int,
            "path": saved_path,
            **metadata,
        },
    )

    app = current_app._get_current_object()
    transcription_enabled = app.config.get("TRANSCRIPTION_ENABLED", True)
    if transcription_enabled and app.config.get("TESTING"):
        _transcribe_and_broadcast(
            app,
            session_id,
            chunk_number_int,
            saved_path,
            metadata,
        )
    elif transcription_enabled:
        socketio.start_background_task(
            _transcribe_and_broadcast,
            app,
            session_id,
            chunk_number_int,
            saved_path,
            metadata,
        )

    return jsonify(
        {
            "message": "Chunk stored",
            "path": saved_path,
            "transcriptionQueued": transcription_enabled,
            **metadata,
        }
    ), 200
