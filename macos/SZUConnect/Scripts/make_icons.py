#!/usr/bin/env python3
"""Render the original geometric app icon, entirely offline."""
from pathlib import Path
from PIL import Image, ImageDraw
import json
root=Path(__file__).resolve().parents[1]/'Assets.xcassets'
icons=root/'AppIcon.appiconset'; icons.mkdir(parents=True,exist_ok=True)
(root/'Contents.json').write_text(json.dumps({'info':{'author':'xcode','version':1}}))
im=Image.new('RGB',(1024,1024),(244,248,246)); d=ImageDraw.Draw(im)
d.rounded_rectangle((112,112,912,912),radius=204,fill=(20,110,94))
# Campus gateway and connection arcs use a consistent stroke weight.
d.arc((254,270,770,786),205,335,fill=(255,255,255),width=47)
d.arc((350,366,674,690),205,335,fill=(255,255,255),width=47)
d.ellipse((481,516,543,578),fill=(255,255,255))
d.line((326,700,326,616),fill=(255,255,255),width=45)
d.line((698,700,698,616),fill=(255,255,255),width=45)
d.line((326,700,698,700),fill=(255,255,255),width=45)
items=[]
for size in [16,32,128,256,512]:
 for scale in [1,2]:
  filename=f'mac-{size}@{scale}x.png'; im.resize((size*scale,size*scale),Image.Resampling.LANCZOS).save(icons/filename)
  items.append({'idiom':'mac','size':f'{size}x{size}','scale':f'{scale}x','filename':filename})
im.save(icons/'ios-1024.png'); items.append({'idiom':'universal','platform':'ios','size':'1024x1024','filename':'ios-1024.png'})
(icons/'Contents.json').write_text(json.dumps({'images':items,'info':{'author':'xcode','version':1}},indent=2))
