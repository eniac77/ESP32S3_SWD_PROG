# -*- coding: utf-8 -*-
"""
render_oled_screens.py — Pixel-pontos OLED-képernyő PNG-generátor.

A cél: a TÉNYLEGES 5x7 fontból (components/display_oled/oled_fonts.c) és a
display_oled.c rajz-primitíveinek PONTOS szemantikájával renderelni mind a 20
UI-képernyőt, ahogy a ui.c rajzolja őket. Minden PNG SCALE=6 nagyítással
(128x64 -> 768x384), OLED-kinézettel.

KÖVETELMÉNY: CSAK Python STDLIB (zlib, struct). Nincs Pillow/külső csomag —
saját minimál PNG-író készül.

Futtatás (IDF python ajánlott, de bármilyen >=3.8 jó):
    python tools/render_oled_screens.py

Kimenet: reference/oled_screens/NN_nev.png  +  _index.png (kontaktlap).

Magyar kommentek, UTF-8.

Fontos hűségi megjegyzés:
  Az 5x7 font csak 0x20..0x5F-et tartalmaz (NAGYBETŰK + számok + jelek).
  A kisbetűk (a..z, 0x60..0x7A) NEM támogatottak -> az oled_font_glyph()
  szóközre esik vissza. Ezt PONTOSAN így replikáljuk, mert ez a valódi
  hardveres megjelenés (pl. "app.bin" -> a kisbetűk üresen jelennek meg).
"""

import os
import re
import struct
import zlib

# --------------------------------------------------------------------------
# Útvonalak — a script tools/ alatt van, a repó gyökér eggyel feljebb.
# --------------------------------------------------------------------------
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT       = os.path.dirname(SCRIPT_DIR)
FONT_C     = os.path.join(ROOT, "components", "display_oled", "oled_fonts.c")
OUT_DIR    = os.path.join(ROOT, "reference", "oled_screens")

# --------------------------------------------------------------------------
# OLED geometria és megjelenés
# --------------------------------------------------------------------------
OLED_W = 128
OLED_H = 64
SCALE  = 6                 # logikai px -> kimeneti px nagyítás
BEZEL  = 8                 # sötét keret a kép körül (px, már a kimeneti skálán)

# Színek (RGB). OLED-kinézet: sötét háttér, cián bekapcsolt pixel.
COL_BG  = (0x0a, 0x0d, 0x12)   # háttér / kikapcsolt pixel
COL_ON  = (0x59, 0xE6, 0xF5)   # bekapcsolt pixel

# --------------------------------------------------------------------------
# UI layout-konstansok (ui.c-ből — NX80-stílus, scale-2 lista, keret-kijelölés)
# --------------------------------------------------------------------------
UI_HDR_H     = 11           # fejléc-sáv magassága (scale 1)
UI_LIST_SCALE = 2           # listaelemek font-skálája (~14px magas)
UI_VISIBLE   = 3            # egyszerre látható listaelemek száma
UI_ROW_H     = 16           # kijelölés-keret magassága (sor-osztás)
UI_LIST_MAXCH = 10          # max karakter egy listasorban (scale 2)

# A 3 látható listasor szöveg-y koordinátái (ui.c UI_ROW_Y[]).
# A kijelölés-keret a sor-y - 1-től UI_ROW_H magasan rajzolódik.
UI_ROW_Y = (14, 31, 48)


# ==========================================================================
# 1) Font-parse: az oled_fonts.c OLED_FONT5X7[96][5] tömbjének kiolvasása.
# ==========================================================================
def parse_font_5x7(path):
    """
    Visszaad egy dict-et: ascii_kod -> [5 oszlop-bajt].
    A C tömb 96 sorát parse-oljuk; minden sor 5 hex bajt {0x..,...}.
    A 0. index = 0x20 (space), tehát ascii = 0x20 + sor_index.
    """
    with open(path, "r", encoding="utf-8") as f:
        src = f.read()

    # A tömb-törzs kivágása: OLED_FONT5X7[...] = { ... };
    m = re.search(r"OLED_FONT5X7\s*\[\s*96\s*\]\s*\[\s*5\s*\]\s*=\s*\{(.*?)\};",
                  src, re.DOTALL)
    if not m:
        raise RuntimeError("Nem talalom az OLED_FONT5X7 tomb-definiciot a fontban.")
    body = m.group(1)

    # Minden belső { ... } egy karakter 5 bájtja. Soronként/csoportonként.
    glyphs = {}
    code = 0x20
    for grp in re.finditer(r"\{([^{}]*)\}", body):
        nums = re.findall(r"0x[0-9A-Fa-f]+|\d+", grp.group(1))
        if len(nums) != 5:
            # csak az 5-bajtos glyph-csoportokat fogadjuk el
            continue
        cols = [int(n, 16) if n.lower().startswith("0x") else int(n) for n in nums]
        glyphs[code] = cols
        code += 1

    # A font 0x20-tol kezdodik. A teljes nyomtathato ASCII 0x20..0x7E = 95 glyph
    # (a tomb [96]-os, a 0x7F lehet ures). Korabban csak 0x20..0x5F (64) volt
    # inicializalva; a font kiegeszitese ota a kisbetuk (a-z) is renderelnek.
    # Elfogadunk legalabb 64 glyphet; a tartomanyon kivuli kod -> szokoz (0x20).
    if len(glyphs) < 64:
        raise RuntimeError("Tul keves glyph (%d) a fontban -> font-parse hiba."
                           % len(glyphs))
    return glyphs


# ==========================================================================
# 2) Framebuffer + rajz-primitívek (display_oled.c szerint, 1:1).
#    A framebuffer 128x64 logikai pixel; minden cella bool (on/off).
#    Az 'on=False' SÖTÉT pixelt rajzol (inverz szöveghez) — pontosan ahogy
#    a display_oled_pixel(x,y,false) törli a bitet.
# ==========================================================================
class FrameBuffer:
    def __init__(self, glyphs):
        self.g = glyphs
        # bytearray 128*64, 0/1
        self.px = bytearray(OLED_W * OLED_H)

    # --- display_oled_pixel(x,y,on) ---
    def pixel(self, x, y, on):
        if x < 0 or x >= OLED_W or y < 0 or y >= OLED_H:
            return
        self.px[y * OLED_W + x] = 1 if on else 0

    # --- display_oled_fill_rect(x,y,w,h,on) ---
    def fill_rect(self, x, y, w, h, on):
        for yy in range(h):
            for xx in range(w):
                self.pixel(x + xx, y + yy, on)

    # --- display_oled_rect(x,y,w,h) — 1px keret, mindig on=true ---
    def rect(self, x, y, w, h):
        for xx in range(w):
            self.pixel(x + xx, y, True)
            self.pixel(x + xx, y + h - 1, True)
        for yy in range(h):
            self.pixel(x, y + yy, True)
            self.pixel(x + w - 1, y + yy, True)

    # --- oled_font_glyph(c): nem támogatott -> szóköz (0x20) ---
    def _glyph(self, c):
        code = ord(c)
        if code < 0x20 or code > 0x7E:
            code = 0x20
        return self.g[code]

    # --- display_oled_char(x,y,c,scale,on) ---
    # col 0..4, row 0..6; ha bits & (1<<row) -> pixel/rect az on-nal.
    def char(self, x, y, c, scale, on):
        cols = self._glyph(c)
        for col in range(5):
            bits = cols[col]
            for row in range(7):
                if bits & (1 << row):
                    if scale == 1:
                        self.pixel(x + col, y + row, on)
                    else:
                        self.fill_rect(x + col * scale, y + row * scale,
                                       scale, scale, on)

    # --- display_oled_text_width(s,scale) = n*6*scale - scale ---
    @staticmethod
    def text_width(s, scale):
        n = len(s)
        if n == 0:
            return 0
        return n * 6 * scale - scale

    # --- display_oled_text(x,y,s,scale): 6*scale előtolás, mindig on=true ---
    def text(self, x, y, s, scale=1):
        for ch in s:
            self.char(x, y, ch, scale, True)
            x += 6 * scale

    # --- display_oled_text_center(y,s,scale) ---
    def text_center(self, y, s, scale=1):
        w = self.text_width(s, scale)
        x = (OLED_W - w) // 2 if w < OLED_W else 0
        self.text(x, y, s, scale)

    # --- ui_draw_header(title) (ui.c): inverz sáv + sötét szöveg 2,2-nél ---
    def header(self, title):
        self.fill_rect(0, 0, OLED_W, UI_HDR_H, True)
        # cím balra, 2px margó, scale 1, on=false (sötét), max 21 char
        for i, ch in enumerate(title[:21]):
            self.char(2 + i * 6, 2, ch, 1, False)

    # --- kijelölt listasor KERETE (ui.c ui_draw_list, UI_SEL_FRAME=1):
    #     display_oled_rect(0, y-1, 128, UI_ROW_H) — a szöveg külön, normál
    #     (nem inverz) scale 2 rajzolódik a sorhoz. ---
    def list_row_frame(self, y):
        self.rect(0, y - 1, OLED_W, UI_ROW_H)


# ==========================================================================
# 3) Minimál PNG-író — IHDR / IDAT / IEND, soronkénti 0 filter, zlib.
# ==========================================================================
def write_png(path, w, h, rgb):
    """
    rgb: bytes/bytearray, hossza w*h*3 (RGB, soronként balrol jobbra, fentrol le).
    Igazi PNG-t ir: 8 bit/csatorna, truecolor (color type 2).
    """
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        crc = zlib.crc32(tag + data) & 0xFFFFFFFF
        return c + struct.pack(">I", crc)

    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)

    # Soronként 1 filter-bajt (0 = None) + a sor RGB adata.
    raw = bytearray()
    stride = w * 3
    for y in range(h):
        raw.append(0)
        raw += rgb[y * stride:(y + 1) * stride]
    idat = zlib.compress(bytes(raw), 9)

    with open(path, "wb") as f:
        f.write(sig)
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", idat))
        f.write(chunk(b"IEND", b""))


def fb_to_rgb(fb, scale=SCALE, bezel=BEZEL):
    """
    A 128x64 logikai framebufferből SCALE-szeres RGB képet készít, körülötte
    'bezel' px sötét kerettel. Visszaad: (w, h, rgb_bytes).
    """
    inner_w = OLED_W * scale
    inner_h = OLED_H * scale
    w = inner_w + 2 * bezel
    h = inner_h + 2 * bezel
    rgb = bytearray(w * h * 3)

    # Háttérrel (a keret is COL_BG) feltöltés.
    bg = bytes(COL_BG)
    for i in range(w * h):
        rgb[i * 3:i * 3 + 3] = bg

    on = bytes(COL_ON)
    for ly in range(OLED_H):
        for lx in range(OLED_W):
            if fb.px[ly * OLED_W + lx]:
                # a logikai pixel kitölt egy scale x scale blokkot
                px0 = bezel + lx * scale
                py0 = bezel + ly * scale
                for dy in range(scale):
                    base = ((py0 + dy) * w + px0) * 3
                    for dx in range(scale):
                        rgb[base + dx * 3:base + dx * 3 + 3] = on
    return w, h, rgb


# ==========================================================================
# 4) A 20 képernyő rajz-leírása — minden screen egy függvény (fb -> None),
#    a ui.c PONTOS koordinátáival és tartalmával.
# ==========================================================================
def make_screens(glyphs):
    screens = []   # (sorszam, nev, rajzolo_fuggveny)

    def add(name, fn):
        screens.append((name, fn))

    # ui_trunc(): a listasorok/scale-2 szövegek max UI_LIST_MAXCH karakterre
    # csonkolva (ui.c). A renderelt szöveg ennyi karakterig megy.
    def trunc(s, maxch=UI_LIST_MAXCH):
        return s[:maxch]

    # ------------------------------------------------------------------
    # Általános NX80-stílusú lista-rajzoló (ui.c ui_draw_list):
    #   - fejléc (inverz sáv, scale 1) MARAD,
    #   - max UI_VISIBLE (3) elem scale 2-vel, x=2, sor-y UI_ROW_Y-ból,
    #   - 10 karakterre csonkolva,
    #   - a kiválasztott sor KERETTEL kiemelve (rect(0, y-1, 128, UI_ROW_H)),
    #   - a szöveg minden sornál sima (nem inverz) scale 2.
    # 'scroll' = 0 minden mintánál (a mintalisták <= 3 elemet mutatnak).
    # ------------------------------------------------------------------
    def draw_list(fb, title, items, sel, empty_msg=""):
        fb.header(title)
        if len(items) <= 0:
            # Üres lista: nagy (scale 2) középre igazított üzenet (ui.c y=28).
            fb.text_center(28, empty_msg, UI_LIST_SCALE)
            return
        scroll = 0
        for row in range(UI_VISIBLE):
            idx = scroll + row
            if idx >= len(items):
                break
            y = UI_ROW_Y[row]
            txt = trunc(items[idx])
            fb.text(2, y, txt, UI_LIST_SCALE)
            if idx == sel:
                fb.list_row_frame(y)   # keret-kijelölés

    # 01 idle — cím scale 2 középen, állapotsorok scale 2 (y=18,36,52).
    def s_idle(fb):
        fb.text_center(0, "SWD PROG", 2)
        fb.text(2, 18, "WiFi:   --", 2)
        fb.text(2, 36, "Target: --", 2)
        fb.text(2, 52, "Serial: --", 2)
    add("idle", s_idle)

    # Rövidített főmenü (ui.c MENU_ITEMS) — a feliratok <= 10 char, scale 2.
    MENU = ["Program fw", "Cel info", "AVR ISP",
            "Cel konfig", "Elo adat", "Beallitas"]

    # 02 menu_program — kiválasztott: "Program fw" (index 0, keret).
    add("menu_program", lambda fb: draw_list(fb, "Fomenu", MENU, 0))
    # 03 menu_avr — kiválasztott: "AVR ISP" (index 2, keret).
    add("menu_avr", lambda fb: draw_list(fb, "Fomenu", MENU, 2))

    # 04 fwlist (SEL = 0) — max 3 elem, scale 2, keret a 0. sornál.
    add("fwlist", lambda fb: draw_list(fb, "Program firmware",
                                       ["app.bin", "blink.bin", "sensor.bin"], 0))

    # 05 fwlist_empty — nagy (scale 2) üzenet középen (ui.c empty_msg, y=28).
    add("fwlist_empty",
        lambda fb: draw_list(fb, "Program firmware", [], 0, "(nincs fw fajl)"))

    # 06 fwsel (ui.c draw_fwsel): "Selected:" scale 2 (y=16), fájlnév scale 1
    #    (y=34), "OK=flash" scale 2 (y=48).
    def s_fwsel(fb):
        fb.header("Program fw")
        fb.text(2, 16, "Selected:", 2)
        fb.text(2, 34, "app.bin", 1)
        fb.text(2, 48, "OK=flash", 2)
    add("fwsel", s_fwsel)

    # 07 swd_prog (57%) — ui_flash_cb: fázis fejléc, célnév scale 2 (y=16,
    #    csonkolt), "57%" scale 2 (y=34), progress-bar rect(2,52,124,10).
    def s_swd_prog(fb):
        fb.header("Iras")
        fb.text(2, 16, trunc("STM32F411"), UI_LIST_SCALE)
        fb.text(2, 34, "57%", UI_LIST_SCALE)
        bx, by, bw, bh = 2, 52, OLED_W - 4, 10
        fb.rect(bx, by, bw, bh)
        fill = ((bw - 2) * 57) // 100
        if fill > 0:
            fb.fill_rect(bx + 1, by + 1, fill, bh - 2, True)
    add("swd_prog", s_swd_prog)

    # 08 swd_done (ui_start_flash OK ág): "Flash OK" scale 2 (y=26),
    #    "Nyomj gombot" scale 1 (y=52).
    def s_swd_done(fb):
        fb.header("KESZ")
        fb.text_center(26, "Flash OK", UI_LIST_SCALE)
        fb.text_center(52, "Nyomj gombot", 1)
    add("swd_done", s_swd_done)

    # 09 swd_err (ui_start_flash hiba ág): esp_err név scale 1 (y=28),
    #    "Nyomj gombot" scale 1 (y=52).
    def s_swd_err(fb):
        fb.header("HIBA")
        fb.text_center(28, "ESP_ERR_TIMEOUT", 1)
        fb.text_center(52, "Nyomj gombot", 1)
    add("swd_err", s_swd_err)

    # 10 celinfo_detect (ui_start_detect): "Detektalas" scale 2 (y=28).
    def s_celinfo_detect(fb):
        fb.header("Cel info")
        fb.text_center(28, "Detektalas", UI_LIST_SCALE)
    add("celinfo_detect", s_celinfo_detect)

    # 11 celinfo_ok (ui_start_detect siker): célnév scale 2 (y=16, csonkolt),
    #    "DEV:0x431" scale 2 (y=34).
    def s_celinfo_ok(fb):
        fb.header("Cel info")
        fb.text(2, 16, trunc("STM32F411"), UI_LIST_SCALE)
        fb.text(2, 34, "DEV:0x431", UI_LIST_SCALE)
    add("celinfo_ok", s_celinfo_ok)

    # 12 celinfo_none (ui_start_detect hiba): "Nincs cel" scale 2 (y=20),
    #    üzenet scale 1 (y=42), "Nyomj gombot" scale 1 (y=54).
    def s_celinfo_none(fb):
        fb.header("Cel info")
        fb.text_center(20, "Nincs cel", UI_LIST_SCALE)
        fb.text_center(42, "no target", 1)
        fb.text_center(54, "Nyomj gombot", 1)
    add("celinfo_none", s_celinfo_none)

    # 13 celinfo_busy (ui_start_detect foglalt): "Foglalt" scale 2 (y=28).
    def s_celinfo_busy(fb):
        fb.header("Cel info")
        fb.text_center(28, "Foglalt", UI_LIST_SCALE)
    add("celinfo_busy", s_celinfo_busy)

    # 14 placeholder (draw_placeholder, pl. "Elo adat"): "hamarosan" scale 2 (y=28).
    def s_placeholder(fb):
        fb.header("Elo adat")
        fb.text_center(28, "hamarosan", UI_LIST_SCALE)
    add("placeholder", s_placeholder)

    # 15 avr_detect (ui_avr_start_detect): "Detektalas" scale 2 (y=28).
    def s_avr_detect(fb):
        fb.header("AVR ISP")
        fb.text_center(28, "Detektalas", UI_LIST_SCALE)
    add("avr_detect", s_avr_detect)

    # 16 avr_ok (ui_avr_start_detect siker): eszköznév scale 2 (y=14, csonkolt),
    #    "SIG:1E 90 07" scale 1 (y=34), "Flash:1024B" scale 1 (y=44),
    #    "OK=tovabb" scale 1 (y=54).
    def s_avr_ok(fb):
        fb.header("AVR ISP")
        fb.text(2, 14, trunc("ATtiny13"), UI_LIST_SCALE)
        fb.text(2, 34, "SIG:1E 90 07", 1)
        fb.text(2, 44, "Flash:1024B", 1)
        fb.text_center(54, "OK=tovabb", 1)
    add("avr_ok", s_avr_ok)

    # 17 avrlist (SEL = 0) — ui_render SCR_AVRLIST cím "AVR ISP fajl".
    add("avrlist", lambda fb: draw_list(fb, "AVR ISP fajl",
                                        ["blink.hex", "fade.bin", "main.hex"], 0))

    # 18 avrsel (ui.c draw_avrsel): "Selected:" scale 2 (y=16), fájlnév scale 1
    #    (y=34), "OK=flash" scale 2 (y=48).
    def s_avrsel(fb):
        fb.header("AVR ISP")
        fb.text(2, 16, "Selected:", 2)
        fb.text(2, 34, "blink.hex", 1)
        fb.text(2, 48, "OK=flash", 2)
    add("avrsel", s_avrsel)

    # 19 avr_prog (57%) — ui_avr_flash_cb: fájlnév scale 2 (y=16, csonkolt),
    #    "57%" scale 2 (y=34), progress-bar rect(2,52,124,10).
    def s_avr_prog(fb):
        fb.header("Iras")
        fb.text(2, 16, trunc("blink.hex"), UI_LIST_SCALE)
        fb.text(2, 34, "57%", UI_LIST_SCALE)
        bx, by, bw, bh = 2, 52, OLED_W - 4, 10
        fb.rect(bx, by, bw, bh)
        fill = ((bw - 2) * 57) // 100
        if fill > 0:
            fb.fill_rect(bx + 1, by + 1, fill, bh - 2, True)
    add("avr_prog", s_avr_prog)

    # 20 avr_done (ui_avr_start_flash OK): "Flash OK" scale 2 (y=26),
    #    "Nyomj gombot" scale 1 (y=52).
    def s_avr_done(fb):
        fb.header("KESZ")
        fb.text_center(26, "Flash OK", UI_LIST_SCALE)
        fb.text_center(52, "Nyomj gombot", 1)
    add("avr_done", s_avr_done)

    return screens


# ==========================================================================
# 5) Kontaktlap-montázs (_index.png): az összes screen kis méretben rácsban,
#    alá sorszám+név felirat ugyanazzal az 5x7 fonttal.
# ==========================================================================
def build_index(glyphs, rendered):
    """
    rendered: lista [(szam, nev, fb), ...] a logikai framebufferekkel.
    Kis bélyegkép minden screenből (THUMB_SCALE), alatta felirat-sáv.
    """
    THUMB_SCALE = 2                       # 128x64 -> 256x128 bélyeg
    PAD         = 6                       # cellák közti rés
    LABEL_H     = 12                      # felirat-sáv magassága (logikai px font)
    COLS        = 4

    tw = OLED_W * THUMB_SCALE             # bélyeg szélesség
    th = OLED_H * THUMB_SCALE             # bélyeg magasság
    cell_w = tw + PAD * 2
    cell_h = th + LABEL_H + PAD * 2 + 4

    rows = (len(rendered) + COLS - 1) // COLS
    W = COLS * cell_w
    H = rows * cell_h

    rgb = bytearray(W * H * 3)
    bg = bytes(COL_BG)
    for i in range(W * H):
        rgb[i * 3:i * 3 + 3] = bg
    on = bytes(COL_ON)

    def put_px(x, y, color):
        if 0 <= x < W and 0 <= y < H:
            o = (y * W + x) * 3
            rgb[o:o + 3] = color

    # Egy logikai framebuffer kirajzolása THUMB_SCALE-lel adott offszetre.
    def blit_fb(fb, ox, oy):
        for ly in range(OLED_H):
            for lx in range(OLED_W):
                if fb.px[ly * OLED_W + lx]:
                    for dy in range(THUMB_SCALE):
                        for dx in range(THUMB_SCALE):
                            put_px(ox + lx * THUMB_SCALE + dx,
                                   oy + ly * THUMB_SCALE + dy, on)

    # Felirat rajzolása az 5x7 fonttal közvetlenül az RGB-be (scale 1).
    def draw_label(text, ox, oy, maxw):
        x = ox
        for ch in text:
            code = ord(ch)
            if code < 0x20 or code > 0x7E:
                code = 0x20
            cols = glyphs[code]
            for col in range(5):
                bits = cols[col]
                for row in range(7):
                    if bits & (1 << row):
                        put_px(x + col, oy + row, on)
            x += 6
            if x - ox > maxw:
                break

    for i, (num, name, fb) in enumerate(rendered):
        r = i // COLS
        c = i % COLS
        cx = c * cell_w
        cy = r * cell_h
        # bélyeg
        blit_fb(fb, cx + PAD, cy + PAD)
        # felirat: "NN nev" — NAGYBETŰS, mert a font csak 0x20..0x5F
        label = ("%02d %s" % (num, name)).upper()
        draw_label(label, cx + PAD, cy + PAD + th + 4, tw - 2)

    return W, H, rgb


# ==========================================================================
# 6) Fő futás
# ==========================================================================
def main():
    glyphs = parse_font_5x7(FONT_C)
    print("Font parse OK: %d glyph (0x20..)" % len(glyphs))

    os.makedirs(OUT_DIR, exist_ok=True)

    screens = make_screens(glyphs)
    rendered = []   # (szam, nev, fb) az index-montázshoz
    written = []

    for i, (name, fn) in enumerate(screens, start=1):
        fb = FrameBuffer(glyphs)
        fn(fb)
        w, h, rgb = fb_to_rgb(fb)
        fname = "%02d_%s.png" % (i, name)
        path = os.path.join(OUT_DIR, fname)
        write_png(path, w, h, rgb)
        written.append(path)
        rendered.append((i, name, fb))
        print("  [%02d] %-22s -> %s (%dx%d)" % (i, name, fname, w, h))

    # Kontaktlap-montázs
    iw, ih, irgb = build_index(glyphs, rendered)
    ipath = os.path.join(OUT_DIR, "_index.png")
    write_png(ipath, iw, ih, irgb)
    written.append(ipath)
    print("  [--] _index.png montazs -> %dx%d" % (iw, ih))

    print("\nKesz: %d PNG a %s mappaban." % (len(written), OUT_DIR))
    return written


if __name__ == "__main__":
    main()
