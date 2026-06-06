#!/usr/bin/env python3
from __future__ import annotations

import math
import subprocess
import sys
from functools import lru_cache
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont


W = 410
H = 502

BG_TOP = (0x07, 0x11, 0x1F)
BG_BOTTOM = (0x11, 0x1E, 0x33)
BG_EDGE = (0x05, 0x09, 0x12)
CARD_BG = (0x10, 0x1B, 0x2D)
CARD_BG_2 = (0x0F, 0x1C, 0x33)
CARD_BORDER = (0x24, 0x34, 0x51)
TEXT_MAIN = (0xE8, 0xF1, 0xFF)
TEXT_SUB = (0x90, 0xA3, 0xC3)
TEXT_DIM = (0x6F, 0x81, 0x9F)
TEXT_MUTED = (0x55, 0x66, 0x7A)
TEAL = (0x45, 0xD6, 0xC2)
BLUE = (0x5B, 0x8C, 0xFF)
ORANGE = (0xFF, 0xB8, 0x6C)
RED = (0xFF, 0x7B, 0x7B)
PILL_BG = (0x13, 0x20, 0x36)
TEXTAREA_BG = (0x12, 0x1D, 0x30)


def clamp_u8(v: float) -> int:
    return int(max(0, min(255, round(v))))


def mix(c1, c2, t: float):
    return tuple(clamp_u8(c1[i] * (1.0 - t) + c2[i] * t) for i in range(3))


def alpha(rgb, a: int):
    return (*rgb, a)


def fc_match(pattern: str) -> str:
    return subprocess.check_output(
        ["fc-match", "-f", "%{file}\n", pattern],
        text=True,
    ).strip()


REGULAR_FONT = fc_match("Noto Sans CJK SC:style=Regular")
BOLD_FONT = fc_match("Noto Sans CJK SC:style=Bold")


@lru_cache(maxsize=None)
def font(size: int, bold: bool = False):
    path = BOLD_FONT if bold else REGULAR_FONT
    return ImageFont.truetype(path, size=size)


def make_canvas() -> Image.Image:
    img = Image.new("RGBA", (W, H), alpha(BG_TOP, 255))
    px = img.load()
    for y in range(H):
        t = y / max(1, H - 1)
        row = mix(BG_TOP, BG_BOTTOM, t)
        for x in range(W):
            # Slight darkening near the edges for a soft vignette.
            edge_x = abs((x - (W - 1) / 2) / ((W - 1) / 2))
            edge_y = abs((y - (H - 1) / 2) / ((H - 1) / 2))
            vignette = max(edge_x, edge_y)
            factor = 1.0 - 0.09 * (vignette ** 1.7)
            px[x, y] = tuple(clamp_u8(c * factor) for c in row) + (255,)

    overlay = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(overlay)
    d.ellipse((-140, -70, 220, 300), fill=alpha((0x1E, 0x8D, 0x88), 92))
    d.ellipse((240, 340, 520, 640), fill=alpha((0x13, 0x1E, 0x38), 160))
    d.ellipse((275, -100, 520, 180), fill=alpha((0x2A, 0x4F, 0xE5), 42))
    overlay = overlay.filter(ImageFilter.GaussianBlur(48))
    img.alpha_composite(overlay)

    return img


def draw_shadowed_round_rect(
    img: Image.Image,
    box,
    radius: int,
    fill,
    outline=None,
    outline_width: int = 1,
    shadow_offset=(0, 10),
    shadow_blur: int = 18,
    shadow_fill=(0, 0, 0, 90),
):
    shadow = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ds = ImageDraw.Draw(shadow)
    x1, y1, x2, y2 = box
    sx, sy = shadow_offset
    ds.rounded_rectangle((x1 + sx, y1 + sy, x2 + sx, y2 + sy), radius=radius, fill=shadow_fill)
    shadow = shadow.filter(ImageFilter.GaussianBlur(shadow_blur))
    img.alpha_composite(shadow)

    d = ImageDraw.Draw(img)
    d.rounded_rectangle(box, radius=radius, fill=fill, outline=outline, width=outline_width)


def text_bbox(draw: ImageDraw.ImageDraw, text: str, fnt):
    return draw.textbbox((0, 0), text, font=fnt)


def text_size(draw: ImageDraw.ImageDraw, text: str, fnt):
    b = text_bbox(draw, text, fnt)
    return b[2] - b[0], b[3] - b[1]


def wrap_text(draw: ImageDraw.ImageDraw, text: str, fnt, max_width: int):
    lines = []
    for paragraph in text.split("\n"):
        if paragraph == "":
            lines.append("")
            continue
        cur = ""
        for ch in paragraph:
            candidate = cur + ch
            if text_size(draw, candidate, fnt)[0] <= max_width or not cur:
                cur = candidate
            else:
                lines.append(cur)
                cur = ch
        if cur:
            lines.append(cur)
    return lines


def draw_wrapped_text(
    draw: ImageDraw.ImageDraw,
    xy,
    text: str,
    fnt,
    fill,
    max_width: int,
    line_spacing: int = 4,
):
    x, y = xy
    lines = wrap_text(draw, text, fnt, max_width)
    bbox = draw.multiline_textbbox((x, y), "\n".join(lines), font=fnt, spacing=line_spacing)
    draw.multiline_text((x, y), "\n".join(lines), font=fnt, fill=fill, spacing=line_spacing)
    return bbox[3]


def draw_centered_text(draw: ImageDraw.ImageDraw, box, text: str, fnt, fill):
    x1, y1, x2, y2 = box
    w, h = text_size(draw, text, fnt)
    draw.text(((x1 + x2 - w) / 2, (y1 + y2 - h) / 2 - 1), text, font=fnt, fill=fill)


def draw_pill_button(
    img: Image.Image,
    box,
    text: str,
    fill,
    text_fill,
    fnt,
    radius: int | None = None,
    shadow: bool = False,
):
    if radius is None:
        radius = int((box[3] - box[1]) / 2)
    if shadow:
        draw_shadowed_round_rect(
            img,
            box,
            radius=radius,
            fill=fill,
            outline=None,
            outline_width=0,
            shadow_offset=(0, 6),
            shadow_blur=10,
            shadow_fill=(0, 0, 0, 45),
        )
    else:
        d = ImageDraw.Draw(img)
        d.rounded_rectangle(box, radius=radius, fill=fill)
    d = ImageDraw.Draw(img)
    draw_centered_text(d, box, text, fnt, text_fill)


def draw_screen_header(draw: ImageDraw.ImageDraw, title: str, subtitle: str):
    draw.text((18, 16), title, font=font(28, True), fill=TEXT_MAIN)
    draw_wrapped_text(draw, (18, 48), subtitle, font(13, False), TEXT_SUB, 374, line_spacing=3)


def draw_card_title_row(draw: ImageDraw.ImageDraw, x: int, y: int, title: str, status: str | None = None):
    draw.text((x, y), title, font=font(17, True), fill=TEXT_MAIN)
    if status:
        w, h = text_size(draw, status, font(12, False))
        draw.text((x + 350 - w, y + 2), status, font=font(12, False), fill=TEXT_SUB)


def draw_status_bar(img: Image.Image, wifi: str, time_text: str, battery: str):
    bar = (16, 10, 394, 38)
    draw_shadowed_round_rect(img, bar, 14, alpha((0x0D, 0x18, 0x29), 220), outline=CARD_BORDER, outline_width=1, shadow_offset=(0, 4), shadow_blur=10, shadow_fill=(0, 0, 0, 22))
    d = ImageDraw.Draw(img)
    d.text((28, 17), wifi, font=font(12, False), fill=TEXT_SUB)
    tw, _ = text_size(d, time_text, font(15, True))
    bw, _ = text_size(d, battery, font(12, False))
    d.text(((W - tw) / 2, 15), time_text, font=font(15, True), fill=TEXT_MAIN)
    icon_x = 336
    icon_y = 18
    d.rounded_rectangle((icon_x, icon_y, icon_x + 18, icon_y + 10), radius=2, outline=TEXT_SUB, width=1)
    d.rectangle((icon_x + 1, icon_y + 2, icon_x + 11, icon_y + 8), fill=TEAL)
    d.rounded_rectangle((icon_x + 19, icon_y + 3, icon_x + 22, icon_y + 7), radius=1, fill=TEXT_SUB)
    d.text((icon_x + 28, 17), battery, font=font(12, False), fill=TEXT_SUB)


def draw_chip(draw: ImageDraw.ImageDraw, x: int, y: int, text: str, fill=PILL_BG, text_fill=TEXT_SUB):
    pad_x = 10
    pad_y = 5
    fnt = font(11, False)
    tw, th = text_size(draw, text, fnt)
    box = (x, y, x + tw + pad_x * 2, y + th + pad_y * 2 - 1)
    draw.rounded_rectangle(box, radius=999, fill=fill, outline=CARD_BORDER, width=1)
    draw.text((x + pad_x, y + pad_y - 1), text, font=fnt, fill=text_fill)
    return box


def draw_sparkline(img: Image.Image, box, values, stroke=TEAL, fill=(0x45, 0xD6, 0xC2, 48)):
    if not values or len(values) < 2:
        return

    x1, y1, x2, y2 = box
    lo = min(values)
    hi = max(values)
    if hi <= lo:
        hi = lo + 1

    points = []
    for idx, value in enumerate(values):
        t = idx / (len(values) - 1)
        x = x1 + (x2 - x1) * t
        y_t = (value - lo) / (hi - lo)
        y = y2 - (y2 - y1) * y_t
        points.append((x, y))

    overlay = Image.new("RGBA", img.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(overlay)
    area = [(points[0][0], y2)] + points + [(points[-1][0], y2)]
    d.polygon(area, fill=fill)
    d.line(points, fill=stroke + (255,), width=3, joint="curve")
    d.ellipse((points[-1][0] - 3, points[-1][1] - 3, points[-1][0] + 3, points[-1][1] + 3), fill=stroke + (255,))
    img.alpha_composite(overlay)


def scene_home() -> Image.Image:
    img = make_canvas()
    draw_status_bar(img, "Wi-Fi", "16:35", "88%")
    d = ImageDraw.Draw(img)
    crypto_box = (16, 50, 394, 194)
    draw_shadowed_round_rect(img, crypto_box, 22, CARD_BG, outline=CARD_BORDER, outline_width=1)
    d = ImageDraw.Draw(img)
    draw_card_title_row(d, 30, 62, "加密货币行情", "CoinGecko · 实时")
    summary_lines = "BTC  $68,432.18  +1.24%\nETH  $3,740.12  +0.82%\nSOL  $182.45  +3.18%"
    draw_wrapped_text(d, (30, 96), summary_lines, font(15, False), TEXT_MAIN, 188, line_spacing=6)
    chart_box = (214, 96, 364, 180)
    draw_shadowed_round_rect(img, chart_box, 16, alpha((0x0D, 0x18, 0x29), 220), outline=CARD_BORDER, outline_width=1, shadow_offset=(0, 4), shadow_blur=10, shadow_fill=(0, 0, 0, 26))
    draw_sparkline(img, (222, 106, 356, 172), [64800, 64950, 64720, 65110, 65290, 65080, 65420, 65640, 65510, 65780, 65920, 65810])
    d = ImageDraw.Draw(img)
    d.text((298, 188), "更新时间 16:35:39", font=font(12, False), fill=TEXT_SUB)

    voice_box = (16, 202, 394, 316)
    draw_shadowed_round_rect(img, voice_box, 22, CARD_BG_2, outline=(0x2E, 0x5A, 0x85), outline_width=1)
    d = ImageDraw.Draw(img)
    draw_card_title_row(d, 30, 216, "语音聊天")
    draw_wrapped_text(
        d,
        (30, 250),
        "点一下开始语音，全屏聊天。自然人声: alloy",
        font(14, False),
        (0xD9, 0xE7, 0xFF),
        340,
        line_spacing=5,
    )

    draw_pill_button(img, (16, 326, 394, 376), "设置", BLUE, TEXT_MAIN, font(18, True), shadow=True)
    return img


def scene_settings_menu() -> Image.Image:
    img = make_canvas()
    d = ImageDraw.Draw(img)
    draw_screen_header(d, "设置", "首页只保留行情和语音，Wi-Fi / 热点 / 自动待机 / 关屏都收在这里。API 和 Key 继续在手机网页里改。")

    status_box = (16, 84, 394, 176)
    draw_shadowed_round_rect(img, status_box, 22, CARD_BG, outline=CARD_BORDER, outline_width=1)
    d = ImageDraw.Draw(img)
    status_text = "模式: AP+STA\n热点: 开启 · 192.168.4.1\nWi-Fi: Home-WiFi · 10.0.0.52\n状态: 准备就绪"
    draw_wrapped_text(d, (30, 98), status_text, font(14, False), TEXT_MAIN, 342, line_spacing=4)

    btn_card = (16, 186, 394, 492)
    draw_shadowed_round_rect(img, btn_card, 22, CARD_BG, outline=CARD_BORDER, outline_width=1)

    button_specs = [
        ((30, 200, 380, 238), "Wi-Fi 设置", BLUE, TEXT_MAIN),
        ((30, 246, 380, 284), "关闭热点", ORANGE, BG_EDGE),
        ((30, 292, 380, 330), "重启 Wi-Fi", ORANGE, BG_EDGE),
        ((30, 338, 380, 376), "自动待机 60s", BLUE, TEXT_MAIN),
        ((30, 384, 380, 422), "关屏", TEXT_MUTED, TEXT_MAIN),
        ((30, 430, 380, 468), "返回首页", TEXT_MUTED, TEXT_MAIN),
    ]
    for box, text, fill, fg in button_specs:
        draw_pill_button(img, box, text, fill, fg, font(16, True), shadow=False)

    return img


def scene_sleep_settings() -> Image.Image:
    img = make_canvas()
    d = ImageDraw.Draw(img)
    draw_screen_header(d, "自动待机", "输入空闲多少秒后自动熄屏。0 表示关闭自动待机，省电但不会自动黑屏。")

    card = (16, 88, 400, 258)
    draw_shadowed_round_rect(img, card, 22, CARD_BG, outline=CARD_BORDER, outline_width=1)
    d = ImageDraw.Draw(img)
    d.text((30, 102), "自动待机秒数", font=font(13, False), fill=TEXT_MAIN)
    draw_textarea(d, (30, 130, 376, 164), "60", "例如 60，0 关闭自动待机", filled=True)
    tip = "0 表示关闭自动待机。建议 30-300 秒，既省电又不会太频繁熄屏。"
    draw_wrapped_text(d, (30, 176), tip, font(12, False), TEXT_SUB, 346, line_spacing=4)

    action_card = (16, 270, 400, 444)
    draw_shadowed_round_rect(img, action_card, 22, CARD_BG, outline=CARD_BORDER, outline_width=1)
    d = ImageDraw.Draw(img)
    draw_wrapped_text(
        d,
        (30, 284),
        "当前设置为 60 秒，空闲后会自动熄屏省电。",
        font(13, False),
        TEXT_MAIN,
        346,
        line_spacing=4,
    )

    draw_pill_button(img, (30, 332, 204, 384), "保存", TEAL, BG_EDGE, font(15, True), shadow=False)
    draw_pill_button(img, (206, 332, 380, 384), "返回设置", TEXT_MUTED, TEXT_MAIN, font(15, True), shadow=False)

    d.text((30, 396), " ",
           font=font(12, False), fill=TEXT_SUB)
    return img


def draw_textarea(draw: ImageDraw.ImageDraw, box, text: str, placeholder: str, filled: bool = False):
    draw.rounded_rectangle(box, radius=12, fill=TEXTAREA_BG, outline=CARD_BORDER, width=1)
    x1, y1, x2, y2 = box
    if filled and text:
        draw.text((x1 + 12, y1 + 7), text, font=font(13, False), fill=TEXT_MAIN)
    else:
        draw.text((x1 + 12, y1 + 7), placeholder, font=font(13, False), fill=TEXT_SUB)


def scene_wifi_settings() -> Image.Image:
    img = make_canvas()
    d = ImageDraw.Draw(img)
    draw_screen_header(
        d,
        "Wi-Fi 设置",
        "直接在手表上输入路由器名称和密码，保存后会自动重连。密码留空表示保持原值。",
    )

    card = (16, 88, 400, 304)
    draw_shadowed_round_rect(img, card, 22, CARD_BG, outline=CARD_BORDER, outline_width=1)
    d = ImageDraw.Draw(img)
    d.text((30, 102), "Wi-Fi 名称", font=font(13, False), fill=TEXT_MAIN)
    draw_textarea(d, (30, 130, 376, 164), "Home-WiFi", "例如 HomeWiFi", filled=True)
    d.text((30, 176), "Wi-Fi 密码", font=font(13, False), fill=TEXT_MAIN)
    draw_textarea(d, (30, 204, 376, 238), "", "留空表示不修改", filled=False)
    tip = "提示: 留空密码会保留原值。输入新的名称和密码后点“保存并重连”。"
    draw_wrapped_text(d, (30, 246), tip, font(12, False), TEXT_SUB, 346, line_spacing=4)

    action_card = (16, 316, 400, 490)
    draw_shadowed_round_rect(img, action_card, 22, CARD_BG, outline=CARD_BORDER, outline_width=1)
    d = ImageDraw.Draw(img)
    draw_wrapped_text(
        d,
        (30, 330),
        "已经保存了 Wi-Fi，改完后点“保存并重连”即可。",
        font(13, False),
        TEXT_MAIN,
        346,
        line_spacing=4,
    )

    draw_pill_button(img, (30, 364, 204, 416), "保存并重连", TEAL, BG_EDGE, font(15, True), shadow=False)
    draw_pill_button(img, (206, 364, 380, 416), "断开 Wi-Fi", RED, BG_EDGE, font(15, True), shadow=False)
    draw_pill_button(img, (30, 428, 380, 476), "返回设置", TEXT_MUTED, TEXT_MAIN, font(16, True), shadow=False)

    return img


def render_history(draw: ImageDraw.ImageDraw, box, history: str):
    x1, y1, x2, y2 = box
    draw.rounded_rectangle(box, radius=22, fill=CARD_BG, outline=CARD_BORDER, width=1)
    draw_wrapped_text(draw, (x1 + 14, y1 + 12), history, font(14, False), TEXT_MAIN, (x2 - x1) - 28, line_spacing=7)


def scene_chat(active: bool = False) -> Image.Image:
    img = Image.new("RGBA", (W, H), alpha((0x03, 0x07, 0x0D), 255))
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gd.ellipse((-70, -85, 230, 170), fill=alpha((0x0E, 0x7B, 0x67), 70))
    gd.ellipse((260, 380, 510, 610), fill=alpha((0x18, 0x2D, 0x4D), 85))
    glow = glow.filter(ImageFilter.GaussianBlur(42))
    img.alpha_composite(glow)
    d = ImageDraw.Draw(img)

    if active:
        history = "我: 正在听..."
    else:
        history = (
            "我: 今天比特币为什么波动？\n"
            "AI: "
            "主要看两点：美元利率预期和 ETF 资金流。短线波动大时，别追涨，先看 24 小时趋势和成交量。"
        )

    d.rounded_rectangle((26, 22, 384, 438), radius=24, fill=alpha((0x08, 0x13, 0x20), 245),
                        outline=alpha((0x1B, 0x52, 0x43), 230), width=1)
    draw_wrapped_text(d, (44, 38), history, font(22, False), TEXT_MAIN, 322, line_spacing=4)

    draw_pill_button(img, (56, 454, 166, 488), "返回", TEXT_MUTED, TEXT_MAIN, font(14, True), shadow=False)
    draw_pill_button(img, (244, 454, 354, 488), "说话", TEAL, BG_EDGE, font(14, True), shadow=False)

    return img


def scene_sleep() -> Image.Image:
    img = Image.new("RGBA", (W, H), alpha((0x00, 0x00, 0x00), 255))
    overlay = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(overlay)
    d.ellipse((18, -20, 250, 200), fill=alpha((0x10, 0x26, 0x39), 72))
    d.ellipse((160, 260, 420, 560), fill=alpha((0x08, 0x10, 0x1A), 160))
    overlay = overlay.filter(ImageFilter.GaussianBlur(28))
    img.alpha_composite(overlay)

    d = ImageDraw.Draw(img)
    d.text((18, 16), "WatchAI", font=font(20, True), fill=alpha(TEXT_DIM, 150))
    pill_box = (53, 210, 357, 258)
    d.rounded_rectangle(pill_box, radius=24, fill=alpha((0x0A, 0x11, 0x1A), 240), outline=alpha(CARD_BORDER, 80), width=1)
    draw_centered_text(d, pill_box, "屏幕已待机，触摸任意位置唤醒", font(14, False), alpha(TEXT_SUB, 210))
    d.text((126, 285), "自动待机已开启", font=font(12, False), fill=alpha(TEXT_DIM, 160))
    return img


def scene_error() -> Image.Image:
    img = make_canvas()
    d = ImageDraw.Draw(img)
    draw_screen_header(d, "WatchAI", "聊天失败")

    box = (16, 84, 394, 260)
    draw_shadowed_round_rect(img, box, 22, CARD_BG, outline=(0x53, 0x23, 0x2F), outline_width=1)
    d = ImageDraw.Draw(img)
    d.text((30, 100), "AP+STA", font=font(18, True), fill=RED)
    d.text((30, 134), "AI 正在思考中...", font=font(14, False), fill=TEXT_SUB)
    err = "最后错误: API 请求失败：连接超时\nAPI: https://broken-proxy.example/v1/chat/completions\nMODEL: gpt-5.5"
    draw_wrapped_text(d, (30, 174), err, font(13, False), RED, 340, line_spacing=5)
    draw_pill_button(img, (30, 206, 210, 248), "重试", TEAL, BG_EDGE, font(15, True), shadow=False)
    draw_pill_button(img, (220, 206, 380, 248), "返回", TEXT_MUTED, TEXT_MAIN, font(15, True), shadow=False)
    return img


def tile_label(canvas: Image.Image, box, title: str):
    d = ImageDraw.Draw(canvas)
    x1, y1, x2, y2 = box
    label_box = (x1 + 12, y1 + 10, x1 + 92, y1 + 36)
    d.rounded_rectangle(label_box, radius=12, fill=alpha(PILL_BG, 235), outline=alpha(CARD_BORDER, 180), width=1)
    draw_centered_text(d, label_box, title, font(12, True), TEXT_MAIN)


def make_contact_sheet(items):
    thumb_scale = 0.5
    thumb_w = int(W * thumb_scale)
    thumb_h = int(H * thumb_scale)
    cols = 2
    rows = math.ceil(len(items) / cols)
    gap_x = 22
    gap_y = 22
    tile_pad = 12
    label_h = 34
    tile_w = thumb_w + tile_pad * 2
    tile_h = thumb_h + tile_pad * 2 + label_h
    sheet_w = cols * tile_w + (cols + 1) * gap_x
    sheet_h = rows * tile_h + (rows + 1) * gap_y

    sheet = Image.new("RGBA", (sheet_w, sheet_h), alpha(BG_EDGE, 255))
    d = ImageDraw.Draw(sheet)
    for y in range(sheet_h):
        t = y / max(1, sheet_h - 1)
        row = mix(BG_EDGE, (0x0A, 0x14, 0x23), t)
        d.line((0, y, sheet_w, y), fill=tuple(row) + (255,))
    bg_overlay = Image.new("RGBA", (sheet_w, sheet_h), (0, 0, 0, 0))
    dd = ImageDraw.Draw(bg_overlay)
    dd.ellipse((-120, -80, 280, 260), fill=alpha((0x1A, 0x8B, 0x82), 60))
    dd.ellipse((sheet_w - 220, sheet_h - 260, sheet_w + 140, sheet_h + 80), fill=alpha((0x13, 0x1C, 0x35), 150))
    bg_overlay = bg_overlay.filter(ImageFilter.GaussianBlur(42))
    sheet.alpha_composite(bg_overlay)

    for idx, (title, image) in enumerate(items):
        row = idx // cols
        col = idx % cols
        x = gap_x + col * (tile_w + gap_x)
        y = gap_y + row * (tile_h + gap_y)
        tile = (x, y, x + tile_w, y + tile_h)
        draw_shadowed_round_rect(sheet, tile, 22, alpha((0x0B, 0x13, 0x21), 220), outline=alpha(CARD_BORDER, 180), outline_width=1)
        tile_label(sheet, tile, title)
        thumb = image.resize((thumb_w, thumb_h), Image.LANCZOS)
        sheet.alpha_composite(thumb, (x + tile_pad, y + label_h + 8))

    return sheet


def save_png(img: Image.Image, path: Path):
    path.parent.mkdir(parents=True, exist_ok=True)
    img.convert("RGBA").save(path)


def main(argv: list[str]) -> int:
    out_dir = Path(argv[1]) if len(argv) > 1 else Path(__file__).resolve().parents[1] / "eval_screenshots_20260604"
    out_dir.mkdir(parents=True, exist_ok=True)

    scenes = [
        ("home", scene_home()),
        ("chat", scene_chat(False)),
        ("chat_recording", scene_chat(True)),
    ]

    for name, img in scenes:
        save_png(img, out_dir / f"{name}.png")

    contact = make_contact_sheet(
        [
            ("home", scenes[0][1]),
            ("chat", scenes[1][1]),
            ("record", scenes[2][1]),
        ]
    )
    save_png(contact, out_dir / "contact_sheet.png")

    print(f"wrote {len(scenes)} screenshots and contact sheet to {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
