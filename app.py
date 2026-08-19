from flask import Flask, jsonify, render_template, request

from config import Config
from routes.audio import audio_bp
from routes.session import session_bp
from services.logging_utils import configure_logging
from services.websocket import emit_status_update, init_socketio, socketio


def create_app() -> Flask:
    """Create and configure the Flask application."""
    app = Flask(
        __name__,
        template_folder="templates",
        static_folder="static",
    )
    app.config.from_object(Config)

    app.register_blueprint(session_bp)
    app.register_blueprint(audio_bp)

    app.logger = configure_logging(app.config["LOG_DIR"])
    init_socketio(app)

    @app.get("/api/health")
    def health_check():
        emit_status_update(
            "hub_status",
            {
                "status": "online",
                "service": "eLingo Hub MVP",
                "version": "1.0",
            },
        )
        return jsonify(
            {
                "status": "online",
                "service": "eLingo Hub MVP",
                "version": "1.0",
            }
        ), 200

    @app.get("/")
    def index():
        return render_template("index.html")

    @app.errorhandler(404)
    def handle_not_found(error):
        if request.path.startswith("/api/"):
            return jsonify({"error": "Not found"}), 404
        return render_template("index.html"), 404

    @app.errorhandler(405)
    def handle_method_not_allowed(error):
        if request.path.startswith("/api/"):
            return jsonify({"error": "Method not allowed"}), 405
        return jsonify({"error": "Method not allowed"}), 405

    return app


app = create_app()


if __name__ == "__main__":
    socketio.run(
        app,
        host="0.0.0.0",
        port=5000,
        debug=True,
        allow_unsafe_werkzeug=True,
    )
