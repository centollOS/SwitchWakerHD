#!/usr/bin/env python3
"""Makes the app's launcher icon from the game's own icon (game/meta/iconTex.tga, 128x128).

usage: python android/make_icon.py [game_dir]   (default: game)

Writes android/app/local-res (git-ignored: the picture is the game's, like the recompiled code it
stays on your machine): an adaptive icon (Android 8+, the launcher crops the full-bleed picture to
its shape) and square PNGs with rounded corners for launchers without adaptive icons. app/build.gradle
uses it when it exists, the SDL icon otherwise. Needs Pillow (pip install pillow).
"""
import os
import sys

from PIL import Image, ImageDraw

here = os.path.dirname(os.path.abspath(__file__))
game = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "game")
src = os.path.join(game, "meta", "iconTex.tga")
out = os.path.join(here, "app", "local-res")
if not os.path.exists(src):
    sys.exit(f"{src} not found (the extracted game's meta folder)")
art = Image.open(src).convert("RGBA")


def write(path, image):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    image.save(path)
    print("wrote", os.path.relpath(path, here))


# adaptive icon: the picture is the 108 dp background layer (432 px at xxxhdpi); the launcher masks it
write(os.path.join(out, "drawable-nodpi", "ic_game_art.png"), art.resize((432, 432), Image.LANCZOS))
os.makedirs(os.path.join(out, "mipmap-anydpi-v26"), exist_ok=True)
with open(os.path.join(out, "mipmap-anydpi-v26", "ic_game.xml"), "w") as f:
    f.write('<?xml version="1.0" encoding="utf-8"?>\n'
            '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
            '    <background android:drawable="@drawable/ic_game_art" />\n'
            '    <foreground android:drawable="@android:color/transparent" />\n'
            '</adaptive-icon>\n')

# legacy icons (48 dp): the picture with rounded corners
for name, size in (("mdpi", 48), ("hdpi", 72), ("xhdpi", 96), ("xxhdpi", 144), ("xxxhdpi", 192)):
    big = art.resize((size * 4, size * 4), Image.LANCZOS)
    mask = Image.new("L", big.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, big.width - 1, big.height - 1), radius=big.width // 5, fill=255)
    big.putalpha(mask)
    write(os.path.join(out, f"mipmap-{name}", "ic_game.png"), big.resize((size, size), Image.LANCZOS))
