from __future__ import annotations

import logging
import os
import threading
from pathlib import Path
from typing import Any

logger = logging.getLogger(__name__)

_model: Any | None = None
_model_kind: str | None = None
_load_attempted = False
_model_lock = threading.Lock()


def _load_model() -> Any | None:
    """Load a local speech-to-text model once and reuse it."""
    global _load_attempted, _model, _model_kind

    with _model_lock:
        if _load_attempted:
            return _model

        _load_attempted = True
        model_name = os.getenv("ELINGO_TRANSCRIPTION_MODEL", "tiny.en")
        device = os.getenv("ELINGO_TRANSCRIPTION_DEVICE", "cpu")
        compute_type = os.getenv("ELINGO_TRANSCRIPTION_COMPUTE_TYPE", "int8")

        try:
            from faster_whisper import WhisperModel

            _model = WhisperModel(model_name, device=device, compute_type=compute_type)
            _model_kind = "faster-whisper"
            logger.info("Loaded faster-whisper model: %s", model_name)
            return _model
        except ImportError:
            logger.warning("faster-whisper is not installed; transcription is disabled")
        except Exception:
            logger.exception("Unable to load faster-whisper model")

        try:
            import whisper

            _model = whisper.load_model(model_name)
            _model_kind = "openai-whisper"
            logger.info("Loaded openai-whisper model: %s", model_name)
            return _model
        except ImportError:
            logger.warning("openai-whisper is not installed; transcription is disabled")
        except Exception:
            logger.exception("Unable to load openai-whisper model")

        _model = None
        _model_kind = None
        return None


def transcribe_chunk(audio_path: str) -> str:
    """Transcribe one WAV chunk and return plain text.

    Transcription is best-effort by design: upload handling must continue even
    when the local speech model is not installed, cannot load, or rejects audio.
    """
    path = Path(audio_path)
    if not path.exists():
        logger.warning("Cannot transcribe missing audio chunk: %s", audio_path)
        return ""

    try:
        model = _load_model()
        if model is None:
            return ""

        if _model_kind == "faster-whisper":
            segments, _info = model.transcribe(str(path), beam_size=1)
            return " ".join(segment.text.strip() for segment in segments).strip()

        if _model_kind == "openai-whisper":
            result = model.transcribe(str(path), fp16=False)
            return str(result.get("text", "")).strip()
    except Exception:
        logger.exception("Transcription failed for %s", audio_path)

    return ""
