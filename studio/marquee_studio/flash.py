"""Flash On / Flash Off: the sign's units (milliseconds) and the photosensitivity rule."""


# ---- Flash (level "Flash On" / "Flash Off") ---------------------------------------------------------------------
# The sign stores them in milliseconds (menu range 25..6375) and converts to 40 fps panel frames by / 25
# (jackpot_controller_init, 0x30698, into the controller-message flash command 0x0400). WCAG 2.3.1: no more than
# three flashes in any one second, i.e. a cycle of at least 334 ms.
FLASH_MIN_MS, FLASH_MAX_MS, FLASH_SAFE_CYCLE_MS = 25, 6375, 334


def flash_frames(spec):
    """(on, off) in panel frames. Spec keys flash_on_ms / flash_off_ms (as the sign), or the older flash_on /
    flash_off in frames."""
    if "flash_on_ms" in spec or "flash_off_ms" in spec:
        on, off = int(spec.get("flash_on_ms") or 0), int(spec.get("flash_off_ms") or 0)
        return (on // 25 if on else 0), (off // 25 if off else 0)
    return int(spec.get("flash_on", 0)), int(spec.get("flash_off", 0))


def flash_warnings(on_ms, off_ms):
    """Problems with a level's flash times (milliseconds), as text."""
    w = []
    if on_ms and not FLASH_MIN_MS <= on_ms <= FLASH_MAX_MS or off_ms and not FLASH_MIN_MS <= off_ms <= FLASH_MAX_MS:
        w.append(f"flash times must be {FLASH_MIN_MS}..{FLASH_MAX_MS} ms (the sign's menu range)")
    if on_ms and on_ms + max(off_ms, 25) < FLASH_SAFE_CYCLE_MS:
        w.append(f"flashes {1000 / (on_ms + max(off_ms, 25)):.1f} times a second: more than 3 per second can trigger "
                 f"seizures (WCAG 2.3.1); use a cycle of {FLASH_SAFE_CYCLE_MS} ms or more")
    return w
