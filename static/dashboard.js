const socket = window.io ? io() : null;

const views = {
  home: document.getElementById('home-view'),
  join: document.getElementById('join-view'),
  live: document.getElementById('live-view'),
};

const showJoinButton = document.getElementById('show-join');
const joinForm = document.getElementById('join-form');
const sessionCodeInput = document.getElementById('session-code-input');
const joinError = document.getElementById('join-error');
const connectionStatus = document.getElementById('connection-status');
const sessionStatus = document.getElementById('session-status');
const sessionId = document.getElementById('session-id');
const recordingState = document.getElementById('recording-state');
const chunksReceived = document.getElementById('chunks-received');
const uploadStatus = document.getElementById('upload-status');
const lastChunk = document.getElementById('last-chunk');
const transcriptLog = document.getElementById('transcript-log');
const systemLogs = document.getElementById('system-logs');
const playLastChunkButton = document.getElementById('play-last-chunk');
const audioPlayer = document.createElement('audio');

const seenChunks = new Set();
const seenTranscripts = new Set();
const loggedTranscriptions = new Set();
let latestSessionId = null;
let latestChunkFilename = null;
let socketConnected = false;
let pollTimer = null;

function showView(name) {
  Object.entries(views).forEach(([viewName, element]) => {
    element.classList.toggle('hidden', viewName !== name);
  });
}

function appendLog(target, message) {
  const timestamp = new Date().toLocaleTimeString();
  target.textContent += `[${timestamp}] ${message}\n`;
  target.scrollTop = target.scrollHeight;
}

function appendTranscript(text) {
  if (!text) {
    return;
  }

  clearTranscriptPlaceholder();
  const separator = transcriptLog.textContent.trim() ? '\n\n' : '';
  transcriptLog.textContent += `${separator}${text}`;
  transcriptLog.scrollTop = transcriptLog.scrollHeight;
}

function clearTranscriptPlaceholder() {
  if (transcriptLog.classList.contains('empty')) {
    transcriptLog.textContent = '';
    transcriptLog.classList.remove('empty');
  }
}

function setTranscriptPlaceholder(message) {
  if (!transcriptLog.textContent.trim() || transcriptLog.classList.contains('empty')) {
    transcriptLog.textContent = message;
    transcriptLog.classList.add('empty');
  }
}

function normalizeSessionCode(value) {
  return value.trim().toUpperCase();
}

function chunkFilename(chunkNumber) {
  return `chunk${String(chunkNumber).padStart(4, '0')}.wav`;
}

function chunkKey(payload) {
  return `${payload.sessionId || latestSessionId || 'unknown'}:${payload.chunkNumber}`;
}

function resetSessionState(session) {
  latestSessionId = session.sessionId;
  latestChunkFilename = null;
  seenChunks.clear();
  seenTranscripts.clear();
  loggedTranscriptions.clear();
  setTranscriptPlaceholder('Waiting for speech...');
  systemLogs.textContent = '';
  playLastChunkButton.disabled = true;
}

function sessionMatches(payload) {
  return payload && latestSessionId && payload.sessionId === latestSessionId;
}

function recordChunk(payload) {
  if (!sessionMatches(payload)) {
    return;
  }

  const key = chunkKey(payload);
  if (seenChunks.has(key)) {
    return;
  }

  seenChunks.add(key);
  chunksReceived.textContent = String(
    Math.max(Number(chunksReceived.textContent || 0), Number(payload.chunkNumber || 0)),
  );

  const filename = payload.filename || chunkFilename(payload.chunkNumber);
  latestChunkFilename = filename;
  const audioSummary = formatAudioSummary(payload);
  uploadStatus.textContent = payload.silent
    ? `Low audio on chunk ${payload.chunkNumber}`
    : `Chunk ${payload.chunkNumber} uploaded`;
  lastChunk.textContent = filename;
  playLastChunkButton.disabled = false;
  appendLog(systemLogs, `Chunk uploaded: ${filename}${audioSummary}`);
}

function recordTranscript(payload) {
  if (!sessionMatches(payload)) {
    return;
  }

  const key = chunkKey(payload);
  logTranscribed(payload);
  if (seenTranscripts.has(key)) {
    return;
  }

  seenTranscripts.add(key);

  if (payload.transcript) {
    appendTranscript(payload.transcript);
  } else if (payload.silent) {
    setTranscriptPlaceholder(
      'No speech detected in the latest chunk. Check microphone VDD/GND and I2S wiring if this continues.',
    );
  } else {
    setTranscriptPlaceholder('No transcript text yet. Speak clearly near the microphone.');
  }
}

function logTranscribed(payload) {
  const key = chunkKey(payload);
  if (loggedTranscriptions.has(key)) {
    return;
  }

  loggedTranscriptions.add(key);
  if (payload.hasTranscript || payload.transcript) {
    appendLog(systemLogs, `Transcribed: ${chunkFilename(payload.chunkNumber)}`);
    return;
  }

  const audioSummary = formatAudioSummary(payload);
  const message = payload.silent ? 'No speech detected' : 'No transcript text';
  appendLog(systemLogs, `${message}: ${chunkFilename(payload.chunkNumber)}${audioSummary}`);
}

function formatAudioSummary(payload) {
  if (!payload || !Number(payload.sampleCount)) {
    return '';
  }

  const average = Number(payload.averageLevel || 0);
  const peak = Number(payload.peakLevel || 0);
  const rms = Number(payload.rmsLevel || 0);
  const label = payload.silent ? 'low audio' : 'audio ok';
  return ` (${label}, avg ${average}, peak ${peak}, rms ${rms})`;
}

function applySession(session) {
  if (!session) {
    return;
  }

  if (latestSessionId !== session.sessionId) {
    resetSessionState(session);
  }

  sessionId.textContent = session.sessionId;
  sessionStatus.textContent = session.status === 'active' ? 'Recording' : 'Ended';
  recordingState.textContent = session.status === 'active' ? 'Recording' : 'Session ended';
  chunksReceived.textContent = String(session.chunkCount || 0);

  const chunks = Array.isArray(session.chunks) ? session.chunks : [];
  chunks.forEach((chunk) => recordChunk({ ...chunk, sessionId: session.sessionId }));
  chunks.forEach((chunk) => {
    if (chunk.transcribedAt || chunk.transcript) {
      recordTranscript({ ...chunk, sessionId: session.sessionId });
    }
  });

  if (chunks.length === 0) {
    uploadStatus.textContent = session.status === 'active' ? 'Waiting for first chunk' : 'Stopped';
    lastChunk.textContent = 'None';
    setTranscriptPlaceholder('Waiting for speech...');
  } else if (session.status !== 'active') {
    uploadStatus.textContent = 'Stopped';
  }

  if (session.transcript) {
    clearTranscriptPlaceholder();
    transcriptLog.textContent = session.transcript.trim();
  }
}

async function refreshJoinedSession() {
  if (!latestSessionId) {
    return;
  }

  try {
    const response = await fetch(`/api/session/${encodeURIComponent(latestSessionId)}`, {
      cache: 'no-store',
    });
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }

    const session = await response.json();
    connectionStatus.textContent = socketConnected ? 'Connected' : 'Polling';
    applySession(session);
  } catch (error) {
    connectionStatus.textContent = 'Polling failed';
    uploadStatus.textContent = 'Session unavailable';
    appendLog(systemLogs, `Session refresh failed: ${error.message}`);
  }
}

function startPolling() {
  window.clearInterval(pollTimer);
  pollTimer = window.setInterval(refreshJoinedSession, 1000);
}

function joinSocketRoom() {
  if (!socket || !latestSessionId || !socketConnected) {
    return;
  }

  socket.emit('join_session', { sessionId: latestSessionId }, (response) => {
    if (!response || !response.success) {
      connectionStatus.textContent = 'Polling';
      return;
    }

    connectionStatus.textContent = 'Connected';
    applySession(response.session);
  });
}

async function joinSession(code) {
  joinError.textContent = '';

  if (!/^[A-Z0-9]{6}$/.test(code)) {
    joinError.textContent = 'Enter the 6-character code shown on the Hub.';
    return;
  }

  try {
    const response = await fetch(`/api/session/${encodeURIComponent(code)}`, {
      cache: 'no-store',
    });
    if (!response.ok) {
      throw new Error(response.status === 404 ? 'Invalid session code.' : `HTTP ${response.status}`);
    }

    const session = await response.json();
    resetSessionState(session);
    applySession(session);
    showView('live');
    connectionStatus.textContent = socketConnected ? 'Connected' : 'Polling';
    startPolling();
    joinSocketRoom();
  } catch (error) {
    joinError.textContent = error.message;
  }
}

function playLastChunk() {
  if (!latestSessionId || !latestChunkFilename) {
    return;
  }

  audioPlayer.src = `/api/session/chunk/${latestSessionId}/${latestChunkFilename}`;
  audioPlayer.play().catch((error) => {
    appendLog(systemLogs, `Playback failed: ${error.message}`);
  });
}

showJoinButton.addEventListener('click', () => {
  showView('join');
  sessionCodeInput.focus();
});

joinForm.addEventListener('submit', (event) => {
  event.preventDefault();
  joinSession(normalizeSessionCode(sessionCodeInput.value));
});

sessionCodeInput.addEventListener('input', () => {
  sessionCodeInput.value = normalizeSessionCode(sessionCodeInput.value);
});

playLastChunkButton.addEventListener('click', playLastChunk);

if (socket) {
  socket.on('connect', () => {
    socketConnected = true;
    if (connectionStatus) {
      connectionStatus.textContent = latestSessionId ? 'Connected' : 'Ready';
    }
    joinSocketRoom();
  });

  socket.on('disconnect', () => {
    socketConnected = false;
    if (connectionStatus) {
      connectionStatus.textContent = latestSessionId ? 'Polling' : 'Offline';
    }
  });

  socket.on('session_joined', (payload) => {
    if (payload && payload.session) {
      applySession(payload.session);
    }
  });

  socket.on('session_ended', (payload) => {
    if (!sessionMatches(payload)) {
      return;
    }

    sessionStatus.textContent = 'Ended';
    recordingState.textContent = 'Session ended';
    uploadStatus.textContent = 'Stopped';
    appendLog(systemLogs, `Session ended: ${payload.sessionId}`);
  });

  socket.on('chunk_uploaded', (payload) => {
    recordChunk(payload);
  });

  socket.on('chunk_transcribed', (payload) => {
    if (sessionMatches(payload)) {
      logTranscribed(payload);
    }
  });

  socket.on('transcription_broadcast', (payload) => {
    if (sessionMatches(payload)) {
      const suffix = payload.hasTranscript ? '' : ' (empty result)';
      appendLog(systemLogs, `Broadcast: ${chunkFilename(payload.chunkNumber)}${suffix}`);
    }
  });

  socket.on('transcript_update', (payload) => {
    recordTranscript(payload);
  });
} else if (connectionStatus) {
  connectionStatus.textContent = 'Polling';
}
