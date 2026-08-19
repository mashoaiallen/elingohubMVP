from io import BytesIO

import pytest

from app import create_app


@pytest.fixture()
def client(tmp_path):
    app = create_app()
    app.config.update(
        TESTING=True,
        TRANSCRIPTION_ENABLED=False,
        UPLOAD_FOLDER=str(tmp_path),
    )
    with app.test_client() as client:
        yield client


def test_upload_audio_chunk_stores_file_and_updates_session(client, tmp_path):
    client.application.config["UPLOAD_FOLDER"] = str(tmp_path)

    create_response = client.post("/api/session/start")
    session_id = create_response.get_json()["sessionId"]

    response = client.post(
        "/api/session/audio",
        data={
            "sessionId": session_id,
            "chunkNumber": "1",
            "audioFile": (BytesIO(b"fake audio bytes"), "chunk0001.wav"),
        },
        content_type="multipart/form-data",
    )

    assert response.status_code == 200
    payload = response.get_json()
    assert payload["message"] == "Chunk stored"

    stored_files = list((tmp_path / session_id).glob("*.wav"))
    assert len(stored_files) == 1
    assert stored_files[0].name == "chunk0001.wav"

    session_response = client.get(f"/api/session/{session_id}")
    assert session_response.get_json()["chunkCount"] == 1
    assert (tmp_path / session_id / "metadata.json").exists()
    assert (tmp_path / session_id / "transcript.txt").exists()


def test_upload_audio_chunk_appends_transcript(client, tmp_path, monkeypatch):
    client.application.config["UPLOAD_FOLDER"] = str(tmp_path)
    client.application.config["TRANSCRIPTION_ENABLED"] = True
    monkeypatch.setattr("routes.audio.transcribe_chunk", lambda audio_path: "Hello I am Allen.")

    create_response = client.post("/api/session/start")
    session_id = create_response.get_json()["sessionId"]

    response = client.post(
        "/api/session/audio",
        data={
            "sessionId": session_id,
            "chunkNumber": "1",
            "audioFile": (BytesIO(b"fake audio bytes"), "chunk0001.wav"),
        },
        content_type="multipart/form-data",
    )

    assert response.status_code == 200
    session_response = client.get(f"/api/session/{session_id}")
    payload = session_response.get_json()
    assert payload["transcript"] == "Hello I am Allen."
    assert payload["chunks"][0]["transcript"] == "Hello I am Allen."
    assert (tmp_path / session_id / "transcript.txt").read_text(encoding="utf-8") == "Hello I am Allen."


def test_upload_audio_chunk_requires_valid_session(client, tmp_path):
    client.application.config["UPLOAD_FOLDER"] = str(tmp_path)

    response = client.post(
        "/api/session/audio",
        data={
            "sessionId": "INVALID",
            "chunkNumber": "1",
            "audioFile": (BytesIO(b"fake audio bytes"), "chunk0001.wav"),
        },
        content_type="multipart/form-data",
    )

    assert response.status_code == 404
    assert response.get_json()["error"] == "Invalid session ID"
