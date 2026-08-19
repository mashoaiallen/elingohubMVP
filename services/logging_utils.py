from __future__ import annotations

import logging
from pathlib import Path
from typing import Optional


def configure_logging(log_dir: Optional[str] = None) -> logging.Logger:
    """Create a reusable logger for the MVP application."""
    log_path = Path(log_dir or "logs")
    log_path.mkdir(parents=True, exist_ok=True)

    logger = logging.getLogger("elingo_hub")
    logger.setLevel(logging.INFO)
    logger.propagate = False

    if not logger.handlers:
        formatter = logging.Formatter(
            "%(asctime)s %(levelname)s %(message)s",
            datefmt="%Y-%m-%d %H:%M:%S",
        )

        file_handler = logging.FileHandler(log_path / "app.log", encoding="utf-8")
        file_handler.setFormatter(formatter)
        logger.addHandler(file_handler)

        stream_handler = logging.StreamHandler()
        stream_handler.setFormatter(formatter)
        logger.addHandler(stream_handler)

    return logger
