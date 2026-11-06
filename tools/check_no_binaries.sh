#!/bin/sh
# Refuse a commit that would add any binary file (except vendored fonts under third_party/fonts/ and fonts-ui/, documentation
# images under docs/media/ and the built emoji set under firmware/emoji/), any file over 1 MB, or the redacted master
# menu code.
# Installed as .git/hooks/pre-commit (see README); can also be run by hand.
status=0
for f in $(git diff --cached --name-only --diff-filter=AM); do
    [ -f "$f" ] || continue
    if git diff --cached --numstat -- "$f" | grep -q '^-	-	'; then
        case "$f" in
            third_party/fonts/*.png|third_party/fonts/*.ttf|third_party/fonts/*.otf) ;;   # vendored fonts/emoji, listed in THIRD-PARTY.md
            third_party/fonts-ui/*.ttf) ;;                         # umami-marquee-sim's UI fonts, listed in THIRD-PARTY.md
            docs/media/*.webp|docs/media/*.png|docs/media/*.gif) ;;  # documentation images (no sign content)
            firmware/emoji/*/emoji.bin) ;;                          # the built emoji set (image data, docs/NATIVE-EMOJI.md)
            *) echo "refused: binary file $f"; status=1 ;;
        esac
    fi
    size=$(wc -c < "$f")
    case "$f" in third_party/imgui/*) size=0 ;; esac          # vendored source text (imgui.cpp is ~1.1 MB)
    if [ "$size" -gt 1048576 ]; then
        echo "refused: $f is larger than 1 MB"; status=1
    fi
done
# The redacted strings live only in .git/info/redact-patterns (one per line, never committed), so this
# script does not spell them out itself.
pat=.git/info/redact-patterns
if [ ! -s "$pat" ]; then
    echo "warning: $pat is missing; the redacted-string check is skipped"
elif git diff --cached -U0 | grep -F -f "$pat" >/dev/null; then
    echo "refused: a redacted string (see $pat) is in the commit; it must stay in docs/PRIVATE-NOTES.md (local only)"; status=1
fi
exit $status
