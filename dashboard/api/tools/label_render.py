#!/usr/bin/env python3
"""label_render.py — deterministic thermal-label bitmap renderer for SolariNet.

Emits a 1-bit label image (QR + Code128 + up to three human-readable text
lines) to **stdout** as raw PNG or PDF bytes. Diagnostics go to **stderr**.
Invoked by dashboard/api/lib/Label.php via proc_open (the Dig.php precedent).

Transport to the physical printers (MakeID EP53/E1 over BLE, Nemonic MIP-201W
over Wi-Fi) is deliberately OUT of scope here: this tool only produces bytes,
so a LabelTransport can be bolted on later without touching the renderer.

Exit codes (contract §3):
    0  ok
    2  bad args            (argparse)
    3  missing deps        (import qrcode / import barcode / PIL failed)
    4  render error        (geometry too small for Code128, etc.)

Usage:
    label_render.py --data 'AKO-0042' --symbology both \\
        --line1 'Dell PowerEdge R410' --line2 'SN CN779216BF0037' \\
        --line3 'AKO-0042' --width-dots 406 --height-dots 203 --dpi 203 \\
        --format png
"""

import argparse
import io
import sys


def _die(code, msg):
    sys.stderr.write(msg.rstrip("\n") + "\n")
    sys.exit(code)


def parse_args(argv):
    p = argparse.ArgumentParser(
        prog="label_render.py",
        description="Render a SolariNet inventory label to stdout (PNG/PDF bytes).",
    )
    p.add_argument("--data", required=True, help="payload encoded in QR + Code128")
    p.add_argument(
        "--symbology",
        default="both",
        choices=["qr", "code128", "both"],
        help="which symbologies to draw (default: both)",
    )
    p.add_argument("--line1", default="", help="primary human text (bold)")
    p.add_argument("--line2", default="", help="secondary human text")
    p.add_argument("--line3", default="", help="tertiary human text (usually the code)")
    p.add_argument("--width-dots", type=int, default=406, dest="width_dots")
    p.add_argument("--height-dots", type=int, default=203, dest="height_dots")
    p.add_argument("--dpi", type=int, default=203)
    p.add_argument("--margin-dots", type=int, default=8, dest="margin_dots")
    p.add_argument("--format", default="png", choices=["png", "pdf"])
    args = p.parse_args(argv)

    # Sanity clamp (Label.php clamps too; belt + braces so geometry is sane).
    if args.width_dots < 32 or args.height_dots < 32:
        p.error("width-dots/height-dots must be >= 32")
    if args.width_dots > 4096 or args.height_dots > 4096:
        p.error("width-dots/height-dots must be <= 4096")
    if args.margin_dots < 0 or args.margin_dots * 2 >= min(args.width_dots, args.height_dots):
        args.margin_dots = max(0, min(args.margin_dots, min(args.width_dots, args.height_dots) // 4))
    return args


def _import_deps():
    """Import the optional rendering deps; exit 3 (with which one) on failure."""
    missing = []
    mods = {}
    try:
        from PIL import Image, ImageDraw, ImageFont  # noqa: F401

        mods["PIL"] = (Image, ImageDraw, ImageFont)
    except Exception as e:  # pragma: no cover - env dependent
        missing.append("Pillow (PIL): %s" % e)
    try:
        import qrcode  # noqa: F401

        mods["qrcode"] = qrcode
    except Exception as e:  # pragma: no cover - env dependent
        missing.append("qrcode: %s" % e)
    try:
        import barcode  # noqa: F401
        from barcode.writer import ImageWriter  # noqa: F401

        mods["barcode"] = (barcode, ImageWriter)
    except Exception as e:  # pragma: no cover - env dependent
        missing.append("python-barcode: %s" % e)
    if missing:
        _die(
            3,
            "missing label renderer deps:\n  "
            + "\n  ".join(missing)
            + "\ninstall: sudo apt install python3-qrcode python3-barcode",
        )
    return mods


def _font(ImageFont, size):
    """A sized default bitmap font (Pillow >= 10 supports load_default(size=))."""
    size = max(6, int(size))
    try:
        return ImageFont.load_default(size=size)
    except TypeError:
        return ImageFont.load_default()


def _text_w(draw, text, font):
    try:
        box = draw.textbbox((0, 0), text, font=font)
        return box[2] - box[0]
    except Exception:
        return draw.textlength(text, font=font)


def _fit(draw, text, font, max_w):
    """Truncate text with an ellipsis so it fits within max_w px."""
    if not text:
        return ""
    if _text_w(draw, text, font) <= max_w:
        return text
    ell = "…"
    s = text
    while s and _text_w(draw, s + ell, font) > max_w:
        s = s[:-1]
    return (s + ell) if s else ""


def _qr_image(qrcode, Image, data, side):
    qr = qrcode.QRCode(
        border=1,
        error_correction=qrcode.constants.ERROR_CORRECT_M,
        box_size=4,
    )
    qr.add_data(data)
    qr.make(fit=True)
    img = qr.make_image(fill_color="black", back_color="white").convert("1")
    if img.size[0] != side:
        img = img.resize((side, side), Image.NEAREST)
    return img


def _code128_image(barcode, ImageWriter, Image, data, target_w, target_h, dpi):
    writer = ImageWriter()
    try:
        obj = barcode.get("code128", data, writer=writer)
    except Exception as e:
        _die(4, "code128 encode failed: %s" % e)
    opts = {
        "write_text": False,
        "quiet_zone": 1.0,
        "module_height": max(3.0, target_h / (dpi / 25.4) if dpi else 6.0),
        "dpi": dpi or 203,
    }
    try:
        img = obj.render(writer_options=opts)
    except Exception as e:
        _die(4, "code128 render failed: %s" % e)
    img = img.convert("1")
    if target_w < 16 or target_h < 8:
        _die(4, "geometry too small for a Code128 strip")
    img = img.resize((max(1, target_w), max(1, target_h)), Image.NEAREST)
    return img


def render(args, mods):
    Image, ImageDraw, ImageFont = mods["PIL"]
    qrcode = mods["qrcode"]
    barcode, ImageWriter = mods["barcode"]

    W, H, M = args.width_dots, args.height_dots, args.margin_dots
    canvas = Image.new("1", (W, H), 1)  # 1 = white
    draw = ImageDraw.Draw(canvas)

    inner_w = W - 2 * M
    inner_h = H - 2 * M
    if inner_w < 16 or inner_h < 16:
        _die(4, "label geometry too small after margins")

    f1 = _font(ImageFont, H / 6)
    f2 = _font(ImageFont, H / 9)
    f3 = _font(ImageFont, H / 9)
    lines = [(args.line1, f1), (args.line2, f2), (args.line3, f3)]

    def draw_text_column(x, y, col_w, avail_h):
        cy = y
        for text, font in lines:
            if not text:
                continue
            asc = font.size + 3
            if cy + asc > y + avail_h:
                break
            draw.text((x, cy), _fit(draw, text, font, col_w), font=font, fill=0)
            cy += asc
        return cy

    if args.symbology == "both":
        qr_side = inner_h
        qr = _qr_image(qrcode, Image, args.data, qr_side)
        canvas.paste(qr, (M, M))
        right_x = M + qr_side + M
        right_w = W - right_x - M
        if right_w < 24:
            _die(4, "right column too narrow for text + Code128")
        bar_h = max(16, int(inner_h * 0.35))
        text_h = inner_h - bar_h - 2
        draw_text_column(right_x, M, right_w, text_h)
        bar = _code128_image(
            barcode, ImageWriter, Image, args.data, right_w, bar_h, args.dpi
        )
        canvas.paste(bar, (right_x, H - M - bar_h))

    elif args.symbology == "qr":
        qr_side = inner_h
        qr = _qr_image(qrcode, Image, args.data, qr_side)
        canvas.paste(qr, (M, M))
        right_x = M + qr_side + M
        right_w = W - right_x - M
        if right_w < 24:
            _die(4, "right column too narrow for text")
        draw_text_column(right_x, M, right_w, inner_h)

    else:  # code128
        bar_h = max(16, int(inner_h * 0.55))
        text_h = inner_h - bar_h - 2
        draw_text_column(M, M, inner_w, text_h)
        bar = _code128_image(
            barcode, ImageWriter, Image, args.data, inner_w, bar_h, args.dpi
        )
        canvas.paste(bar, (M, H - M - bar_h))

    out = io.BytesIO()
    if args.format == "pdf":
        canvas.convert("1").save(out, format="PDF", resolution=float(args.dpi or 203))
    else:
        canvas.convert("1").save(out, format="PNG", dpi=(args.dpi or 203, args.dpi or 203))
    return out.getvalue()


def main(argv):
    args = parse_args(argv)
    mods = _import_deps()
    try:
        payload = render(args, mods)
    except SystemExit:
        raise
    except Exception as e:  # pragma: no cover - defensive
        _die(4, "render error: %s" % e)
    stdout = getattr(sys.stdout, "buffer", sys.stdout)
    stdout.write(payload)
    stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
