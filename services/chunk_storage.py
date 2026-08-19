from __future__ import annotations

import json
from pathlib import Path
from typing import Dict, Optional, Tuple
from flask import current_app
from werkzeug.datastructures import FileStorage

from services.session_manager import SessionManager, session_manager as shared_session_manager


class ChunkStorage:
    """Persist uploaded audio chunks to the local uploads directory."""

    def __init__(self, session_manager: Optional[SessionManager] = None) -> None:
        self._session_manager = session_manager or shared_session_manager

    def _session_dir(self, session_id: str) -> Path:
        upload_root = Path(current_app.config["UPLOAD_FOLDER"])
        return upload_root / session_id

    def prepare_session(self, session_id: str) -> Path:
        """Create the session directory and baseline metadata files."""
        session = self._session_manager.get_session(session_id)
        if not session:
            raise ValueError("Invalid session ID")

        session_dir = self._session_dir(session_id)
        session_dir.mkdir(parents=True, exist_ok=True)
        (session_dir / "transcript.txt").touch(exist_ok=True)
        self.write_metadata(session_id)
        return session_dir

    def write_metadata(self, session_id: str) -> None:
        """Persist the latest serializable session metadata."""
        snapshot = self._session_manager.session_snapshot(session_id)
        if not snapshot:
            return

        session_dir = self._session_dir(session_id)
        session_dir.mkdir(parents=True, exist_ok=True)
        metadata_path = session_dir / "metadata.json"
        with metadata_path.open("w", encoding="utf-8") as handle:
            json.dump(snapshot, handle, indent=2)

    def append_transcript(
        self,
        session_id: str,
        chunk_number: int,
        transcript: str,
        timestamp: str,
    ) -> None:
        """Append transcribed chunk text and refresh metadata."""
        self._session_manager.record_transcript(
            session_id,
            chunk_number,
            transcript,
            timestamp,
        )

        session_dir = self._session_dir(session_id)
        session_dir.mkdir(parents=True, exist_ok=True)

        if transcript:
            transcript_path = session_dir / "transcript.txt"
            separator = "\n\n" if transcript_path.exists() and transcript_path.stat().st_size else ""
            with transcript_path.open("a", encoding="utf-8") as handle:
                handle.write(f"{separator}{transcript}")

        self.write_metadata(session_id)

    def _parse_wav_metadata(self, file_path: Path, filename: str) -> Dict[str, object]:
        metadata: Dict[str, object] = {
            "filename": filename,
            "sampleRate": 0,
            "bitDepth": 0,
            "channels": 0,
            "duration": 0.0,
            "sampleCount": 0,
            "averageLevel": 0,
            "peakLevel": 0,
            "rmsLevel": 0,
            "silent": False,
        }

        try:
            with file_path.open("rb") as handle:
                header = handle.read(44)
                if len(header) < 44:
                    return metadata

                metadata["channels"] = int.from_bytes(header[22:24], "little")
                metadata["sampleRate"] = int.from_bytes(header[24:28], "little")
                metadata["bitDepth"] = int.from_bytes(header[34:36], "little")
                data_size = int.from_bytes(header[40:44], "little")
                metadata["duration"] = float(data_size) / (
                    metadata["sampleRate"]
                    * (metadata["bitDepth"] // 8)
                    * metadata["channels"]
                )

                audio_bytes = handle.read()
                if metadata["bitDepth"] == 16 and metadata["channels"] == 1:
                    sample_count = len(audio_bytes) // 2
                    metadata["sampleCount"] = sample_count
                    if sample_count > 0:
                        total_abs = 0
                        total_square = 0
                        peak = 0
                        for sample_index in range(sample_count):
                            sample = int.from_bytes(
                                audio_bytes[sample_index * 2 : sample_index * 2 + 2],
                                "little",
                                signed=True,
                            )
                            absolute_sample = abs(sample)
                            total_abs += absolute_sample
                            total_square += sample * sample
                            peak = max(peak, absolute_sample)
                        metadata["averageLevel"] = int(total_abs / sample_count)
                        metadata["peakLevel"] = peak
                        metadata["rmsLevel"] = int((total_square / sample_count) ** 0.5)
                        metadata["silent"] = (
                            metadata["averageLevel"] < 32
                            and metadata["peakLevel"] < 128
                        )
        except (OSError, ValueError):
            pass

        return metadata

    def store_chunk(
        self,
        session_id: str,
        chunk_number: str,
        audio_file: FileStorage,
    ) -> Tuple[str, Dict[str, object]]:
        """Store a chunk under uploads/<session_id>/ and return its path and metadata."""
        session = self._session_manager.get_session(session_id)
        if not session:
            raise ValueError("Invalid session ID")

        if session.status != "active":
            raise ValueError("Session is not active")

        session_dir = self.prepare_session(session_id)

        filename = f"chunk{int(chunk_number):04d}.wav"
        destination = session_dir / filename
        audio_file.save(destination)

        metadata = self._parse_wav_metadata(destination, filename)
        self._session_manager.record_chunk(
            session_id,
            int(chunk_number),
            str(destination),
            metadata,
        )
        self.write_metadata(session_id)
        return str(destination), metadata
