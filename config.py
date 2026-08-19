import os
from pathlib import Path


class Config:
    """Application configuration for the MVP prototype."""

    SECRET_KEY = "elingo-hub-dev-secret"
    UPLOAD_FOLDER = str(Path(__file__).resolve().parent / "uploads")
    LOG_DIR = str(Path(__file__).resolve().parent / "logs")
    MAX_CONTENT_LENGTH = 16 * 1024 * 1024
    TRANSCRIPTION_ENABLED = (
        os.getenv("ELINGO_TRANSCRIPTION_ENABLED", "true").strip().lower()
        not in {"0", "false", "no", "off"}
    )
