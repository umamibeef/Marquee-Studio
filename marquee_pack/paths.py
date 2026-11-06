"""Where things live. Every module asks here instead of working paths out from its own location.

REPO          the repository root
CONTENT       the studio's content folder (fonts, animations, config): $MARQUEE_STUDIO_CONTENT, else studio/content
THIRD_PARTY   vendored third-party files (THIRD-PARTY.md); FONTS inside it
EMOJI_PACK    the bought emoji pack (local only, never committed): $MARQUEE_EMOJI_PACK, else local/emoji-pack
FIRMWARE_APP  the extracted ELITE_RGB application, if present (used for LED correction tables; never shipped)
"""
import os

PKG = os.path.dirname(os.path.abspath(__file__))        # marquee_pack/, at the repo root
REPO = os.path.dirname(PKG)
STUDIO = os.path.join(REPO, "studio")
CONTENT = os.environ.get("MARQUEE_STUDIO_CONTENT") or os.path.join(STUDIO, "content")
THIRD_PARTY = os.path.join(REPO, "third_party")
FONTS = os.path.join(THIRD_PARTY, "fonts")
# A bought emoji pack that may not be redistributed: on this machine only (git-ignored local/)
EMOJI_PACK = os.environ.get("MARQUEE_EMOJI_PACK") or os.path.join(REPO, "local", "emoji-pack")
FIRMWARE_APP = os.path.join(REPO, "analysis", "extracted", "ELITE_RGB")
