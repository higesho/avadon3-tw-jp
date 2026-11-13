"""Redraw the English text in the Avadon 3 UI images in Japanese.

usage:
    python -I make_images.py ORIGINAL_DIR OUTPUT_DIR [PREVIEW_DIR] [NAME ...]

ORIGINAL_DIR: the untouched images (_backup_original/Graphics Core).
Each image keeps its size, format and (for 8-bit BMPs) its palette; only the boxes
listed in SPEC are erased and redrawn, so clickable areas stay where they were.
"""
import os
import sys

import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

FONTS = 'C:/Windows/Fonts/'
MINCHO_B = FONTS + 'yumindb.ttf'       # Yu Mincho Demibold
GOTHIC_B = FONTS + 'YuGothB.ttc'       # Yu Gothic Bold
UD_GOTHIC_B = FONTS + 'BIZ-UDGothicB.ttc'

INK = (24, 16, 12)          # black-brown text of the table images
RED_INK = (92, 20, 16)      # dark red of the tutorial images
STONE_INK = (40, 32, 30)    # stats screen labels

# erase: 'clear' (transparent), 'fill' (median border colour), 'inpaint' (stone texture)
# text entries: (box, lines, font, max_size, colour, align)
CREAM = (237, 224, 195)     # help images (G169, G159)

SPEC = {
    'G158.png': {   # title menu (silver letters)
        'erase': 'clear-all',
        'style': 'silver',
        'texts': [((10, y0, 302, y1), [t], MINCHO_B, 30, None, 'center') for (y0, y1), t in [
            ((13, 44), '新しいゲーム'), ((59, 91), 'ロード'), ((107, 138), '操作説明'),
            ((154, 191), '設定'), ((199, 231), 'メダル'), ((247, 284), '終了')]]},
    'G203.png': {   # in-game menu (transparent background, same layout as Avadon 1)
        'erase': 'fill', 'pad': 2, 'fill_colour': (0, 0, 0, 0),
        'texts': [((14, y0 - 1, 276, y1 + 1), [t], MINCHO_B, 18, (214, 190, 178), 'center', 'shadow') for (y0, y1), t in [
            ((6, 22), 'ゲームに戻る'), ((61, 76), 'セーブ'), ((88, 105), 'セーブデータをロード'),
            ((144, 163), 'ゲーム設定'), ((170, 191), 'キーボードショートカット'),
            ((226, 243), 'タイトル画面に戻る'), ((254, 272), '終了')]]},
    'G220.png': {   # sequel notice
        'erase': 'fill', 'pad': 1, 'fill_colour': (0, 0, 0, 0),
        'texts': [((5, 2, 400, 23), ['冒険は続く……'], MINCHO_B, 20, (252, 244, 214), 'left', 'shadow')]},
    'G167.png': {   # main screen diagram labels
        'erase': 'fill', 'pad': 1, 'fill_colour': (0, 0, 0, 0),
        'texts': [((219, y0, 399, y1), [t], GOTHIC_B, 15, (240, 202, 129), 'left', 'shadow') for (y0, y1), t in [
            ((32, 48), 'A - 地形画面'), ((55, 70), 'B - ボタン'), ((77, 95), 'C - パーティー'),
            ((99, 118), 'D - 自動地図')]]},
    'G197.png': {   # inventory diagram labels
        'erase': 'fill', 'pad': 1, 'fill_colour': (0, 0, 0, 0),
        'texts': [((217, y0, 411, y1), [t], GOTHIC_B, 15, (222, 214, 164), 'left', 'shadow') for (y0, y1), t in [
            ((22, 38), 'A - 地面のアイテム'), ((45, 63), 'B - 装備中のアイテム'), ((68, 86), 'C - クイック使用'),
            ((91, 106), 'D - 持ち物'), ((114, 129), 'E - スカラベ')]]},
    'G169.png': {   # how to attack
        'erase': 'fill', 'pad': 0, 'fill_colour': (0, 0, 0, 0),
        'clear_rects': [(286, 197, 297, 217)],
        'texts': [
            ((140, 0, 330, 21), ['攻撃のしかた'], MINCHO_B, 18, CREAM, 'center', 'shadow'),
            ((184, 50, 286, 93), ['この敵を', '攻撃するには……'], GOTHIC_B, 15, CREAM, 'center', 'shadow'),
            ((184, 150, 286, 217), ['……敵が立って', 'いるマスを', '選択します。'], GOTHIC_B, 15, CREAM, 'center', 'shadow'),
        ]},
    'G159.png': {   # how to use quick-use items
        'erase': 'fill', 'pad': 0, 'fill_colour': (0, 0, 0, 0),
        'texts': [((6, 22, 113, 86), ['このボタンで', 'アイテムを', '使います。'], GOTHIC_B, 17, CREAM, 'left', 'shadow')]},
    'G128.png': {   # panel headers (map, roster) and loading bars
        'erase': 'recolor',
        'texts': [
            ((818, 7, 858, 21), ['地図'], MINCHO_B, 13, (226, 214, 212), 'center', 'shadow'),
            ((782, 504, 850, 518), ['仲間'], MINCHO_B, 13, (226, 214, 212), 'center', 'shadow'),
            ((125, 598, 215, 613), ['読み込み中'], GOTHIC_B, 12, (170, 160, 150), 'center', 'shadow'),
            ((125, 634, 215, 649), ['読み込み中'], GOTHIC_B, 12, (232, 220, 218), 'center', 'shadow'),
        ]},
}


def fit_font(path, lines, box, max_size):
    w, h = box[2] - box[0], box[3] - box[1]
    for size in range(max_size, 7, -1):
        f = ImageFont.truetype(path, size)
        widths = [f.getbbox(t)[2] for t in lines]
        lh = size + max(1, size // 8)
        if max(widths) <= w and lh * len(lines) - (lh - size) <= h + 2:
            return f, size, lh
    f = ImageFont.truetype(path, 8)
    return f, 8, 9


def text_layer(size, box, lines, font_path, max_size, align):
    """Alpha mask (L) of the text placed in box."""
    f, s, lh = fit_font(font_path, lines, box, max_size)
    mask = Image.new('L', size, 0)
    d = ImageDraw.Draw(mask)
    total = lh * len(lines) - (lh - s)
    y = box[1] + (box[3] - box[1] - total) / 2
    for t in lines:
        bb = f.getbbox(t)
        tw = bb[2]
        if align == 'center':
            x = box[0] + (box[2] - box[0] - tw) / 2
        elif align == 'right':
            x = box[2] - tw
        else:
            x = box[0]
        d.text((x, y - bb[1] + (s - (bb[3] - bb[1])) / 2), t, font=f, fill=255)
        y += lh
    return mask


def border_colour(arr, box):
    x0, y0, x1, y1 = box
    edge = np.concatenate([arr[y0, x0:x1], arr[y1, x0:x1], arr[y0:y1, x0], arr[y0:y1, x1]])
    if (edge[:, 3] < 128).mean() > 0.5:      # transparent background
        return (0, 0, 0, 0)
    edge = edge[edge[:, 3] >= 128]
    return tuple(int(v) for v in np.median(edge, axis=0))


def gradient_fill(size, mask, top, bottom, box):
    w, h = size
    grad = Image.new('RGB', size)
    gd = ImageDraw.Draw(grad)
    y0, y1 = box[1], box[3]
    for y in range(h):
        t = min(1, max(0, (y - y0) / max(1, y1 - y0)))
        c = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
        gd.line([(0, y), (w, y)], fill=c)
    out = Image.new('RGBA', size, (0, 0, 0, 0))
    out.paste(grad, (0, 0), mask)
    return out


def render(name, src_dir):
    spec = SPEC[name]
    src = Image.open(os.path.join(src_dir, name))
    palette_img = src if src.mode == 'P' else None
    img = src.convert('RGBA')
    size = img.size
    arr = np.array(img)

    if spec['erase'] == 'clear-all':
        img = Image.new('RGBA', size, (0, 0, 0, 0))
    elif spec['erase'] == 'fill':
        pad = spec.get('pad', 0)
        out_arr = arr.copy()
        for t in spec['texts']:
            x0, y0, x1, y1 = t[0]
            x0, y0 = max(0, x0 - pad), max(0, y0 - pad)
            x1, y1 = min(size[0] - 1, x1 + pad), min(size[1] - 1, y1 + pad)
            c = spec.get('fill_colour') or border_colour(arr, (x0, y0, x1, y1))
            blk = arr[y0:y1 + 1, x0:x1 + 1]
            sub = (blk[:, :, :3].astype(int).sum(axis=2) < 300) & (blk[:, :, 3] > 128)
            grid_rows = sub.mean(axis=1) > 0.7     # table rules: keep them
            grid_cols = sub.mean(axis=0) > 0.7
            for yy in range(y0, y1 + 1):
                if grid_rows[yy - y0]:
                    continue
                for xx in range(x0, x1 + 1):
                    if not grid_cols[xx - x0]:
                        out_arr[yy, xx] = c
        img = Image.fromarray(out_arr, 'RGBA')
    elif spec['erase'] == 'recolor':
        # light text on a dark label: paint the light pixels (and a 1px rim) with the
        # median colour of the dark pixels of the same box
        out_arr = arr.copy()
        for t in spec['texts']:
            x0, y0, x1, y1 = t[0]
            blk = arr[y0:y1, x0:x1]
            g = blk[:, :, :3].astype(int).mean(axis=2)
            thr = np.median(g) + 22
            dark = blk[g < thr]
            c = np.median(dark, axis=0).astype(np.uint8)
            m = (g >= thr).astype(np.uint8) * 255
            m = cv2.dilate(m, np.ones((3, 3), np.uint8), iterations=1) > 0
            sub = out_arr[y0:y1, x0:x1]
            sub[m] = c
        img = Image.fromarray(out_arr, 'RGBA')
    elif spec['erase'] == 'inpaint':
        alpha = np.array(img)[:, :, 3]
        rgb = cv2.cvtColor(np.array(img.convert('RGB')), cv2.COLOR_RGB2BGR)
        m = np.zeros(rgb.shape[:2], np.uint8)
        gray = cv2.cvtColor(rgb, cv2.COLOR_BGR2GRAY)
        for t in spec['texts']:
            x0, y0, x1, y1 = t[0]
            light = len(t) > 6 and t[6] == 'outline'
            sub = gray[y0:y1, x0:x1]
            hit = (sub > 130) | (sub < 60) if light else (sub < 120)
            m[y0:y1, x0:x1] = hit.astype(np.uint8) * 255
        m = cv2.dilate(m, np.ones((3, 3), np.uint8), iterations=1)
        rgb = cv2.inpaint(rgb, m, 4, cv2.INPAINT_TELEA)
        out = np.dstack([cv2.cvtColor(rgb, cv2.COLOR_BGR2RGB), alpha])
        img = Image.fromarray(out.astype(np.uint8), 'RGBA')

    for x0, y0, x1, y1 in spec.get('clear_rects', []):
        ImageDraw.Draw(img).rectangle([x0, y0, x1 - 1, y1 - 1], fill=(0, 0, 0, 0))
    style = spec.get('style')
    for t in spec['texts']:
        box, lines, font_path, max_size, colour, align = t[:6]
        mask = text_layer(size, box, lines, font_path, max_size, align)
        if len(t) > 6 and t[6] == 'shadow':
            sh = mask.filter(ImageFilter.MaxFilter(3))
            img.alpha_composite(Image.composite(Image.new('RGBA', size, (16, 10, 6, 220)),
                                                Image.new('RGBA', size, (0, 0, 0, 0)), sh), (1, 1))
            img.alpha_composite(Image.composite(Image.new('RGBA', size, colour + (255,)),
                                                Image.new('RGBA', size, (0, 0, 0, 0)), mask))
            continue
        if len(t) > 6 and t[6] == 'outline':
            edge = mask.filter(ImageFilter.MaxFilter(3))
            img.alpha_composite(Image.composite(Image.new('RGBA', size, (0, 0, 0, 255)),
                                                Image.new('RGBA', size, (0, 0, 0, 0)), edge))
            img.alpha_composite(Image.composite(Image.new('RGBA', size, colour + (255,)),
                                                Image.new('RGBA', size, (0, 0, 0, 0)), mask))
            continue
        if style == 'gold':
            # dark outline + shadow, then gold gradient (colours sampled from the original menu)
            shadow = mask.filter(ImageFilter.MaxFilter(5)).filter(ImageFilter.GaussianBlur(1.5))
            img.alpha_composite(Image.composite(Image.new('RGBA', size, (20, 12, 4, 230)),
                                                Image.new('RGBA', size, (0, 0, 0, 0)), shadow.point(lambda v: min(255, v * 2))),
                                (1, 2))
            edge = mask.filter(ImageFilter.MaxFilter(3))
            img.alpha_composite(Image.composite(Image.new('RGBA', size, (58, 40, 18, 255)),
                                                Image.new('RGBA', size, (0, 0, 0, 0)), edge))
            img.alpha_composite(gradient_fill(size, mask, (250, 236, 190), (176, 136, 70), box))
        elif style == 'silver':
            # Avadon 3 title menu: dark outline + soft shadow, white-to-taupe gradient
            shadow = mask.filter(ImageFilter.MaxFilter(5)).filter(ImageFilter.GaussianBlur(1.5))
            img.alpha_composite(Image.composite(Image.new('RGBA', size, (12, 8, 4, 220)),
                                                Image.new('RGBA', size, (0, 0, 0, 0)), shadow.point(lambda v: min(255, v * 2))),
                                (1, 2))
            edge = mask.filter(ImageFilter.MaxFilter(3))
            img.alpha_composite(Image.composite(Image.new('RGBA', size, (40, 28, 18, 255)),
                                                Image.new('RGBA', size, (0, 0, 0, 0)), edge))
            img.alpha_composite(gradient_fill(size, mask, (246, 241, 240), (176, 152, 132), box))
        elif style == 'blue':
            img.alpha_composite(gradient_fill(size, mask, (110, 120, 200), (40, 44, 110), box))
        else:
            layer = Image.new('RGBA', size, colour + (255,))
            img.alpha_composite(Image.composite(layer, Image.new('RGBA', size, (0, 0, 0, 0)), mask))

    if palette_img is not None:
        out = img.convert('RGB').quantize(palette=palette_img, dither=Image.Dither.NONE)
    elif name.lower().endswith('.jpg'):
        out = img.convert('RGB')
    else:
        out = img
    return out


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    src_dir, out_dir = argv[0], argv[1]
    preview = argv[2] if len(argv) > 2 and not argv[2].startswith('G') else None
    names = [a for a in argv[2:] if a.startswith('G')] or list(SPEC)
    for name in names:
        out = render(name, src_dir)
        p = os.path.join(out_dir, name)
        if name.lower().endswith('.jpg'):
            out.save(p, quality=95)
        else:
            out.save(p)
        if preview:
            pv = out.convert('RGBA')
            bg = Image.new('RGBA', pv.size, (90, 90, 90, 255))
            bg.alpha_composite(pv)
            bg.convert('RGB').save(os.path.join(preview, name.rsplit('.', 1)[0] + '_jp.png'))
        print('wrote', p)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
