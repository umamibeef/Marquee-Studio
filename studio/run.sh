#!/bin/sh
# Launch Umami Marquee Studio, then open http://127.0.0.1:8750 in your browser.
cd "$(dirname "$0")"
exec ../venv/bin/python server.py
