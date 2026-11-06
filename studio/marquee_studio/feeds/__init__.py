"""Data feeds for data apps (docs/APPS.md): each module's fetch(settings) returns {key: text} for the app; the
Studio's server sends the values to the sign over the DB9 (console "adata") while it is connected."""
import importlib

FEEDS = {"weather": 600}                    # feed: seconds between fetches


def fetch(name, settings):
    return importlib.import_module(f".{name}", __name__).fetch(settings or {})
