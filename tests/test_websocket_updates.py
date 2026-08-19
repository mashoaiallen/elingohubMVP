from app import create_app
from services.websocket import emit_status_update, socketio
from io import BytesIO


def test_emit_status_update_delivers_event_to_socket_clients():
    app = create_app()
    app.config.update(TESTING=True, TRANSCRIPTION_ENABLED=False)

    test_client = socketio.test_client(app)
    emit_status_update("session_created", {"sessionId": "ABC123"})

    received_events = test_client.get_received()
    assert len(received_events) == 1
    assert received_events[0]["name"] == "session_created"
    assert received_events[0]["args"] == [{"sessionId": "ABC123"}]


def test_transcript_update_only_reaches_joined_session_clients(tmp_path, monkeypatch):
    app = create_app()
    app.config.update(
        TESTING=True,
        TRANSCRIPTION_ENABLED=True,
        UPLOAD_FOLDER=str(tmp_path),
    )
    monkeypatch.setattr("routes.audio.transcribe_chunk", lambda audio_path: "Live words.")

    flask_client = app.test_client()
    start_response = flask_client.post("/api/session/start")
    session_id = start_response.get_json()["sessionId"]

    joined_client = socketio.test_client(app)
    other_client = socketio.test_client(app)
    joined_client.emit("join_session", {"sessionId": session_id})
    joined_client.get_received()
    other_client.get_received()

    response = flask_client.post(
        "/api/session/audio",
        data={
            "sessionId": session_id,
            "chunkNumber": "1",
            "audioFile": (BytesIO(b"fake audio bytes"), "chunk0001.wav"),
        },
        content_type="multipart/form-data",
    )

    assert response.status_code == 200

    joined_events = joined_client.get_received()
    other_events = other_client.get_received()
    joined_transcripts = [
        event for event in joined_events if event["name"] == "transcript_update"
    ]
    other_transcripts = [
        event for event in other_events if event["name"] == "transcript_update"
    ]

    assert joined_transcripts
    assert joined_transcripts[0]["args"][0]["transcript"] == "Live words."
    assert other_transcripts == []
