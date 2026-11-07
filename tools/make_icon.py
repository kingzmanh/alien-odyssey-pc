"""Dev-build icon from the game's emblem picture (INTRO\DATA\A.LBM on the CD).
usage: make_icon.py A.LBM out.ico"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lbm import load
from PIL import Image

im = load(sys.argv[1]).convert('RGBA')
px = im.load()
for y in range(im.height):
    for x in range(im.width):
        r, g, b, a = px[x, y]
        if r + g + b < 60:
            px[x, y] = (0, 0, 0, 0)
bb = im.getbbox()
im = im.crop(bb)
side = max(im.size)
side += side // 16
sq = Image.new('RGBA', (side, side), (0, 0, 0, 0))
sq.paste(im, ((side - im.width) // 2, (side - im.height) // 2))
sq.save(sys.argv[2], sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
