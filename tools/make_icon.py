"""make_icon.py ALIENS.ICO out.ico : the game's 32x32 icon (from the user's CD)
as a multi-size Windows icon, scaled with sharp pixels."""
import sys
from PIL import Image
src = Image.open(sys.argv[1]).convert('RGBA')
# crop away the empty (transparent) part so the logo fills the icon
bbox = src.getbbox() or (0, 0, 32, 32)
logo = src.crop(bbox)
w, h = logo.size
side = max(w, h)
sq = Image.new('RGBA', (side, side), (0, 0, 0, 0))
sq.paste(logo, ((side - w) // 2, (side - h) // 2))
big = sq.resize((256, 256), Image.NEAREST)
big.save(sys.argv[2], sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
print('icon', src.size, 'logo bbox', bbox)
