"""The weather (Open-Meteo, https://open-meteo.com: free, no account or key) for the weather app: temp, feels, hi, lo
(rounded whole degrees in the app's units), wind (km/h or mph), sky (an emoji from the sign's emoji set, or a word
when the set lacks it) and sky_text."""
import json
import urllib.parse
import urllib.request

# WMO weather codes (Open-Meteo's weather_code) -> (emoji, word)
SKY = [((0,), "☀", "Clear"), ((1,), "\U0001F324", "Mostly clear"), ((2,), "⛅", "Partly cloudy"),
       ((3,), "☁", "Cloudy"), ((45, 48), "\U0001F32B", "Fog"), ((51, 53, 55, 56, 57), "\U0001F326", "Drizzle"),
       ((61, 63, 65, 66, 67, 80, 81, 82), "\U0001F327", "Rain"), ((71, 73, 75, 77, 85, 86), "\U0001F328", "Snow"),
       ((95, 96, 99), "⛈", "Storm")]


def sky(code):
    """(emoji or word, word) for a WMO code; the emoji only when the sign's emoji set has it (firmware/emoji/set.txt
    and the imported emoji: what the sign's emoji.bin carries), else the word, never a character the sign lacks"""
    from .. import native_emoji
    on_sign = set(native_emoji.available())
    for codes, emoji, word in SKY:
        if code in codes:
            return (emoji if native_emoji.key(emoji) in on_sign else word), word
    return "", "Unknown"


def fetch(settings):
    imperial = str(settings.get("units", "celsius")).startswith("f")
    q = {"latitude": settings.get("latitude", "49.28"), "longitude": settings.get("longitude", "-123.12"),
         "current": "temperature_2m,apparent_temperature,weather_code,wind_speed_10m",
         "daily": "temperature_2m_max,temperature_2m_min", "timezone": "auto", "forecast_days": 1,
         "temperature_unit": "fahrenheit" if imperial else "celsius", "wind_speed_unit": "mph" if imperial else "kmh"}
    url = "https://api.open-meteo.com/v1/forecast?" + urllib.parse.urlencode(q)
    with urllib.request.urlopen(url, timeout=10) as r:
        d = json.load(r)
    c, day = d["current"], d["daily"]
    em, word = sky(int(c["weather_code"]))
    return {"temp": str(round(c["temperature_2m"])), "feels": str(round(c["apparent_temperature"])),
            "hi": str(round(day["temperature_2m_max"][0])), "lo": str(round(day["temperature_2m_min"][0])),
            "wind": str(round(c["wind_speed_10m"])), "sky": em, "sky_text": word}
