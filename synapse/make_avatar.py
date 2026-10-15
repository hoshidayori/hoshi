#!/usr/bin/env python3
"""Draws a small test avatar: a night sky with a few stars, 128x128 PNG. Synapse makes thumbnails with
Pillow, so tests need a real image rather than arbitrary bytes.
Usage: make_avatar.py OUTPUT.png"""
import random
import sys

from PIL import Image, ImageDraw

SIZE = 128
image = Image.new('RGB', (SIZE, SIZE))
draw = ImageDraw.Draw(image)
for y in range(SIZE):
    # From deep blue at the top to violet at the bottom
    t = y / (SIZE - 1)
    draw.line([(0, y), (SIZE, y)], fill=(int(20 + 90 * t), int(24 + 20 * t), int(70 + 80 * t)))
stars = random.Random(1018)  # fixed, so the file is the same every time
for _ in range(14):
    x, y, r = stars.randrange(SIZE), stars.randrange(SIZE), stars.choice([1, 1, 2])
    draw.ellipse([x - r, y - r, x + r, y + r], fill=(255, 244, 214))
draw.ellipse([78, 22, 104, 48], fill=(255, 236, 170))
image.save(sys.argv[1], optimize=True)
