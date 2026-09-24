"""Resize an existing LVGL 8.x TRUE_COLOR_ALPHA C array, in place.

The weather icons arrived as generated C, not as PNGs, so there is no original
to go back to -- this reads the pixels out of the array, scales them and writes
the file back.

Only the LV_COLOR_DEPTH==16 / LV_COLOR_16_SWAP==0 variant is read, and only that
variant is written: it is what lib/lv_conf.h pins, and the three other blocks the
original generator emitted were dead weight in the source. The output #errors on
any other configuration, matching tools/png2lvgl.py.

Why not a smaller pixel format instead of a smaller image? Because LVGL hands a
TRUE_COLOR_ALPHA variable straight to the blitter as a pointer into flash and
allocates nothing (lv_img_decoder.c, lv_img_decoder_built_in_open). Every
INDEXED_* format instead calls lv_mem_alloc three times behind LV_ASSERT_MALLOC,
which on this board restarts the panel. Fewer pixels is the only way to spend
less flash without spending heap.

Usage: python tools/lvglresize.py <file.c> <new_size>
"""
import io
import re
import sys

from PIL import Image

BLOCK = "#if LV_COLOR_DEPTH == 16 && LV_COLOR_16_SWAP == 0"


def read_source(path):
    text = io.open(path, encoding="utf-8", errors="replace").read()

    array = re.search(r"uint8_t\s+(\w+)_map\s*\[\]", text)
    descriptor = re.search(
        r"const\s+lv_img_dsc_t\s+(\w+)\s*=\s*\{\s*\{\s*(\w+),\s*\d+,\s*\d+,\s*(\d+),\s*(\d+),",
        text,
    )
    if not array or not descriptor:
        sys.exit("%s: could not find the array or the descriptor" % path)
    if descriptor.group(2) != "LV_IMG_CF_TRUE_COLOR_ALPHA":
        sys.exit("%s: only TRUE_COLOR_ALPHA is handled, found %s" % (path, descriptor.group(2)))

    # Files from the online converter carry four #if variants; ours carry one.
    if BLOCK in text:
        start = text.index(BLOCK) + len(BLOCK)
        body = text[start:text.index("#endif", start)]
    else:
        start = text.index("_map[] = {")
        body = text[start:text.index("};", start)]

    data = [int(b, 16) for b in re.findall(r"0x([0-9a-fA-F]{2})", body)]
    width, height = int(descriptor.group(3)), int(descriptor.group(4))
    if len(data) != width * height * 3:
        sys.exit("%s: expected %d bytes, read %d" % (path, width * height * 3, len(data)))

    note = re.search(r"(//\s*img ID = \d+)", text)
    return array.group(1), descriptor.group(1), width, height, data, note.group(1) if note else None


def to_image(width, height, data):
    """RGB565 little-endian + A8 -> RGBA, expanding by bit replication, which is
    exactly what lv_color_make inverts when the panel draws it back."""
    image = Image.new("RGBA", (width, height))
    pixels = image.load()
    for index in range(width * height):
        low, high, alpha = data[index * 3], data[index * 3 + 1], data[index * 3 + 2]
        colour = low | (high << 8)
        r5, g6, b5 = (colour >> 11) & 0x1F, (colour >> 5) & 0x3F, colour & 0x1F
        pixels[index % width, index // width] = (
            (r5 << 3) | (r5 >> 2),
            (g6 << 2) | (g6 >> 4),
            (b5 << 3) | (b5 >> 2),
            alpha,
        )
    return image


def resize(image, size):
    """Through premultiplied alpha. These icons are transparent black outside
    the artwork, and resizing straight RGBA would average that black into every
    edge pixel and ring the whole icon with a dark halo."""
    source = image.load()
    premultiplied = Image.new("RGBA", image.size)
    target = premultiplied.load()
    for y in range(image.size[1]):
        for x in range(image.size[0]):
            r, g, b, a = source[x, y]
            target[x, y] = (r * a // 255, g * a // 255, b * a // 255, a)

    premultiplied = premultiplied.resize((size, size), Image.LANCZOS)

    scaled = premultiplied.load()
    out = Image.new("RGBA", (size, size))
    result = out.load()
    for y in range(size):
        for x in range(size):
            r, g, b, a = scaled[x, y]
            if a == 0:
                result[x, y] = (0, 0, 0, 0)
            else:
                result[x, y] = (
                    min(255, r * 255 // a),
                    min(255, g * 255 // a),
                    min(255, b * 255 // a),
                    a,
                )
    return out


def write_source(path, array, descriptor, size, image, note, original):
    pixels = image.load()
    rows = []
    for y in range(size):
        row = []
        for x in range(size):
            r, g, b, a = pixels[x, y]
            c565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            row.append("0x%02x, 0x%02x, 0x%02x," % (c565 & 0xFF, (c565 >> 8) & 0xFF, a))
        rows.append("    " + " ".join(row))

    guard = array.upper()
    identifier = "%s\n" % note if note else ""
    io.open(path, "w", encoding="utf-8", newline="\n").write(
        """// AUTO-GENERATED. Do not hand-edit.
// Rescaled from %d x %d to %d x %d by tools/lvglresize.py, to fit the firmware
// into an OTA slot. See platformio.ini for the flash budget this serves.

#include <lvgl.h>

#if LV_COLOR_DEPTH != 16 || LV_COLOR_16_SWAP != 0
#error "%s was generated for LV_COLOR_DEPTH 16 with LV_COLOR_16_SWAP 0."
#endif

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

#ifndef LV_ATTRIBUTE_IMG_%s
#define LV_ATTRIBUTE_IMG_%s
#endif

const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMG_%s
uint8_t %s_map[] = {
%s
};

%sconst lv_img_dsc_t %s = {
    {
        LV_IMG_CF_TRUE_COLOR_ALPHA,
        0,
        0,
        %d,
        %d,
    },
    %d * LV_IMG_PX_SIZE_ALPHA_BYTE,
    %s_map,
};
"""
        % (
            original[0], original[1], size, size,
            array, guard, guard, guard, array,
            "\n".join(rows),
            identifier, descriptor, size, size, size * size, array,
        )
    )


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    path, size = sys.argv[1], int(sys.argv[2])
    array, descriptor, width, height, data, note = read_source(path)
    image = resize(to_image(width, height, data), size)
    write_source(path, array, descriptor, size, image, note, (width, height))
    print("%-48s %dx%d -> %dx%d  %6d -> %6d bytes"
          % (path.rsplit("/", 1)[-1], width, height, size, size,
             width * height * 3, size * size * 3))


main()
