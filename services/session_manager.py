from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Dict, List, Optional
import random
import string
import threading


@dataclass
class SessionRecord:
    """Represents the state of a single recording session."""

    session_id: str
    created_at: datetime = field(default_factory=lambda: datetime.now(timezone.utc))
    status: str = "active"
    chunk_count: int = 0
    chunks: List[Dict[str, object]] = field(default_factory=list)
    transcript: str = ""


class SessionManager:
    """Manage the lifecycle of temporary local sessions."""

    def __init__(self) -> None:
        self._sessions: Dict[str, SessionRecord] = {}
        self._latest_session_id: str | None = None
        self._lock = threading.RLock()

    def create_session(self) -> str:
        """Create a new session ID and register it."""
        with self._lock:
            session_id = self._generate_session_id()
            while session_id in self._sessions:
                session_id = self._generate_session_id()

            self._sessions[session_id] = SessionRecord(session_id=session_id)
            self._latest_session_id = session_id
            return session_id

    def get_session(self, session_id: str) -> SessionRecord | None:
        """Return a session record if it exists."""
        with self._lock:
            return self._sessions.get(session_id)

    def get_latest_session(self) -> SessionRecord | None:
        """Return the newest session record if any session exists."""
        with self._lock:
            if not self._latest_session_id:
                return None

            return self._sessions.get(self._latest_session_id)

    def end_session(self, session_id: str) -> bool:
        """Mark a session as closed if it exists."""
        with self._lock:
            session = self._sessions.get(session_id)
            if not session:
                return False

            session.status = "ended"
            return True

    def increment_chunk_count(self, session_id: str) -> bool:
        """Increase the chunk count for an active session."""
        with self._lock:
            session = self._sessions.get(session_id)
            if not session or session.status != "active":
                return False

            session.chunk_count += 1
            return True

    def record_chunk(
        self,
        session_id: str,
        chunk_number: int,
        path: str,
        metadata: Optional[Dict[str, object]] = None,
    ) -> bool:
        """Record a stored chunk for dashboard state."""
        with self._lock:
            session = self._sessions.get(session_id)
            if not session or session.status != "active":
                return False

            session.chunk_count += 1
            chunk_record = {
                "chunkNumber": chunk_number,
                "path": path,
            }
            if metadata:
                chunk_record.update(metadata)

            session.chunks.append(chunk_record)
            return True

    def record_transcript(
        self,
        session_id: str,
        chunk_number: int,
        transcript: str,
        timestamp: str,
    ) -> bool:
        """Attach transcribed text to a chunk and append it to the session text."""
        with self._lock:
            session = self._sessions.get(session_id)
            if not session:
                return False

            for chunk in session.chunks:
                if chunk.get("chunkNumber") == chunk_number:
                    chunk["transcript"] = transcript
                    chunk["transcribedAt"] = timestamp
                    break

            if transcript:
                if session.transcript:
                    session.transcript += "\n\n"
                session.transcript += transcript

            return True

    def latest_snapshot(self) -> Dict[str, object]:
        """Return a dashboard-friendly snapshot of the newest session."""
        with self._lock:
            session = self.get_latest_session()
            if not session:
                return {
                    "session": None,
                }

            return {
                "session": self.session_snapshot(session.session_id),
            }

    def session_snapshot(self, session_id: str) -> Dict[str, object] | None:
        """Return a serializable snapshot for one session."""
        with self._lock:
            session = self._sessions.get(session_id)
            if not session:
                return None

            return {
                "sessionId": session.session_id,
                "status": session.status,
                "chunkCount": session.chunk_count,
                "createdAt": session.created_at.isoformat(),
                "chunks": list(session.chunks),
                "transcript": session.transcript,
            }

    def _generate_session_id(self) -> str:
        """Generate a short alpha-numeric session identifier."""
        return "".join(random.choices(string.ascii_uppercase + string.digits, k=6))


session_manager = SessionManager()
