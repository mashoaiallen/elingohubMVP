from io import BytesIO

import pytest
from pathlib import Path

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


def test_start_session_returns_session_id(client):
    response = client.post("/api/session/start")

    assert response.status_code == 200
    payload = response.get_json()
    assert "sessionId" in payload
    assert len(payload["sessionId"]) == 6


def test_session_status_can_be_retrieved_and_ended(client):
    start_response = client.post("/api/session/start")
    session_id = start_response.get_json()["sessionId"]

    status_response = client.get(f"/api/session/{session_id}")
    assert status_response.status_code == 200
    assert status_response.get_json()["status"] == "active"

    end_response = client.post("/api/session/end", json={"sessionId": session_id})
    assert end_response.status_code == 200

    final_status_response = client.get(f"/api/session/{session_id}")
    assert final_status_response.get_json()["status"] == "ended"


def test_chunk_playback_route_serves_saved_file(client, tmp_path):
    client.application.config["UPLOAD_FOLDER"] = str(tmp_path)

    create_response = client.post("/api/session/start")
    session_id = create_response.get_json()["sessionId"]

    upload_response = client.post(
        "/api/session/audio",
        data={
            "sessionId": session_id,
            "chunkNumber": "1",
            "audioFile": (BytesIO(b"RIFF....WAVEfmt "), "chunk0001.wav"),
        },
        content_type="multipart/form-data",
    )

    assert upload_response.status_code == 200

    chunk_response = client.get(f"/api/session/chunk/{session_id}/chunk0001.wav")
    assert chunk_response.status_code == 200
    assert chunk_response.content_type == "audio/wav"
    assert chunk_response.data.startswith(b"RIFF")


def test_invalid_session_id_returns_not_found(client):
    response = client.post("/api/session/end", json={"sessionId": "INVALID"})

    assert response.status_code == 404


def test_health_endpoint_returns_online_status(client):
    response = client.get("/api/health")

    assert response.status_code == 200
    assert response.is_json
    assert response.content_type.startswith("application/json")
    payload = response.get_json()
    assert payload["status"] == "online"
    assert payload["service"] == "eLingo Hub MVP"
    assert payload["version"] == "1.0"
