"""The sign's money formatter (amount_format, ELITE_RGB 0x479B4) and Value fields."""


# ---- Money amounts (amount_format, ELITE_RGB 0x479B4; checked against the firmware by test_amount.py) ----------
AMOUNT_MODES = {1: "plain", 0: "odometer digits", 2: "odometer digits, small cents", 3: "plain, small cents"}


def amount_format(value, decimals=2, mode=1, dec=".", thou=",", prefix="$", suffix=""):
    """The sign's money text for value in minor units (e.g. cents), as the firmware formats it.
    Modes: 1 plain ASCII; 0 odometer digits (codes 0x10..0x19) and a lone '$' prefix -> 0x1C; 2 odometer digits
    with small cents (codes 0x00..0x09); 3 ASCII with small cents. thou "" (or "\\0") means no separator.
    Returns bytes in the sign's font codes."""
    pre = prefix.encode("latin1")
    if mode in (0, 2) and pre == b"$":
        pre = b"\x1c"
    d = bytearray(str(value & 0xFFFFFFFF).rjust(decimals + 1, "0").encode())
    n = len(d)
    if mode in (2, 3):
        for i in range(n - decimals, n):
            d[i] -= 0x30
    if mode in (0, 2):
        for i in range(n - decimals):
            d[i] -= 0x20
    if mode == 0:
        for i in range(n - decimals, n):
            d[i] -= 0x20
    k, ln = 0, n
    if decimals:
        d.insert(ln - decimals, ord(dec))
        k, ln = decimals + 1, ln + 1
    if thou and thou != "\0":
        u = k + 3
        while u < ln:
            d.insert(ln - u, ord(thou))
            u, ln = u + 4, ln + 1
    return pre + bytes(d) + suffix.encode("latin1")

def amount_text(value, opts):
    """A Value field as the sign shows it: "1250000", "12,500.00" or 12500 -> amount_format with opts
    (decimals, mode, dec, thou, prefix, suffix), as a str of font codes (chr(0x10) etc.)."""
    o = {"decimals": 2, "mode": 1, "dec": ".", "thou": ",", "prefix": "$", "suffix": ""}
    o.update({k: v for k, v in opts.items() if k in o})
    d, m = int(o["decimals"]), int(o["mode"])
    s = str(value).strip().replace(o["thou"] or "\0", "") if isinstance(value, str) else value
    if isinstance(s, str):
        s = s.replace(o["prefix"], "").replace(o["dec"], ".") if o["prefix"] else s.replace(o["dec"], ".")
    minor = round(float(s) * 10 ** d)
    if not 0 <= minor < 2 ** 32:
        raise ValueError("amount out of range (0 .. 4294967295 in minor units)")
    return amount_format(minor, d, m, o["dec"], o["thou"], o["prefix"], o["suffix"]).decode("latin1")
