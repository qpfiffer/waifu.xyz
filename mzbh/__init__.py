from flask import Flask


def create_app():
    from mzbh import settings
    from mzbh.blueprints import main
    from mzbh.cache import cache
    from mzbh.commands import command_init
    from mzbh.database import db_init
    from mzbh.utils import template_init

    app = Flask(__name__, instance_relative_config=True)
    app.config.from_mapping(
        SECRET_KEY=settings.SECRET_KEY,
        CACHE_TYPE="simple",
        CACHE_DEFAULT_TIMEOUT=300,
    )

    app.config['SQLALCHEMY_TRACK_MODIFICATIONS'] = False

    app.register_blueprint(main.blueprint)

    cache.init_app(app)
    command_init(app)
    db_init(app)
    template_init(app)

    return app
