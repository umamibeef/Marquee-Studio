"""Named colours (RGB555) and colour parsing."""


# Named colours from the firmware (RGB555 constants) + common full-bright values.
PALETTE = {
    "Red": 0x7C00, "Green": 0x03E0, "Yellow": 0x7FE0, "Amber": 0x7E80, "Orange": 0x7D00,
    "Dim Red": 0x3C00, "Dim Green": 0x01E0, "Dim Yellow": 0x3DE0, "Gold": 0x7EA0,
    "White": 0x7FFF, "Blue": 0x001F, "Cyan": 0x03FF, "Magenta": 0x7C1F, "Off": 0x0000,
}

def _colour(v):
    if isinstance(v, str):
        if v in PALETTE:
            return PALETTE[v]
        return int(v, 16)
    return int(v) & 0xFFFF
