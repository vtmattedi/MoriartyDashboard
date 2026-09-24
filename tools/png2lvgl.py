"""Convert a PNG to an LVGL 8.x TRUE_COLOR_ALPHA C array.

Only the LV_COLOR_DEPTH==16 / LV_COLOR_16_SWAP==0 variant is emitted -- that is
what lib/lv_conf.h pins -- and the output #errors on any other configuration so
a future lv_conf change fails loudly instead of rendering garbage.
"""
import sys
from PIL import Image

SRC = sys.argv[1]
DST = sys.argv[2]
NAME = sys.argv[3]
SIZE = int(sys.argv[4])

# The MW mark is two-tone: orange for one half, near-black for the other. The
# dark half would disappear on the dashboard's dark background, so anything
# darker than this luminance is remapped to a light slate. Antialiased edges
# carry partial alpha and get remapped the same way, so the outline stays clean.
DARK_LUMA_MAX = 110
LIGHT = (222, 230, 241)

im = Image.open(SRC).convert("RGBA")
im = im.resize((SIZE, SIZE), Image.LANCZOS)

px = im.load()
for y in range(SIZE):
    for x in range(SIZE):
        r, g, b, a = px[x, y]
        if a == 0:
            continue
        luma = (299 * r + 587 * g + 114 * b) // 1000
        if luma <= DARK_LUMA_MAX:
            px[x, y] = (LIGHT[0], LIGHT[1], LIGHT[2], a)

rows = []
for y in range(SIZE):
    out = []
    for x in range(SIZE):
        r, g, b, a = px[x, y]
        c565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out.append("0x%02x, 0x%02x, 0x%02x," % (c565 & 0xFF, (c565 >> 8) & 0xFF, a))
    rows.append("    " + " ".join(out))

guard = NAME.upper()
body = "\n".join(rows)
data_size = SIZE * SIZE

with open(DST, "w", encoding="utf-8") as f:
    f.write(f"""// AUTO-GENERATED from the NightMare logo art. Do not hand-edit.
// Source: MwNightmareSystem/frontend/src/assets/newlogo/logo_image_only.png
// Regenerate with tools/png2lvgl.py if the mark or the target size changes.
//
// The dark half of the two-tone mark is remapped to a light slate so it stays
// visible on the dashboard's dark background.

#include <lvgl.h>

#if LV_COLOR_DEPTH != 16 || LV_COLOR_16_SWAP != 0
#error "{NAME} was generated for LV_COLOR_DEPTH 16 with LV_COLOR_16_SWAP 0. Regenerate it with tools/png2lvgl.py."
#endif

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

#ifndef LV_ATTRIBUTE_IMG_{guard}
#define LV_ATTRIBUTE_IMG_{guard}
#endif

const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMG_{guard}
uint8_t {NAME}_map[] = {{
{body}
}};

const lv_img_dsc_t {NAME} = {{
    {{
        LV_IMG_CF_TRUE_COLOR_ALPHA,
        0,
        0,
        {SIZE},
        {SIZE},
    }},
    {data_size} * LV_IMG_PX_SIZE_ALPHA_BYTE,
    {NAME}_map,
}};
""")

print(f"wrote {DST}: {SIZE}x{SIZE}, {data_size * 3} bytes of pixel data")
