#!/usr/bin/env python3
"""Convert local music and optional sidecars into CYD's low-memory SD format."""
import argparse
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('source', type=Path, help='Folder containing music files')
parser.add_argument('destination', type=Path, help='SD card root or staging folder')
parser.add_argument('--earbuds', help='Exact Bluetooth advertised device name')
args = parser.parse_args()
if not args.source.is_dir():
    parser.error('source must be an existing folder')
if not shutil.which('ffmpeg'):
    parser.error('ffmpeg is required; install it first')
out = args.destination / 'music'
out.mkdir(parents=True, exist_ok=True)
if args.earbuds:
    (args.destination / 'bluetooth.txt').write_text(args.earbuds.strip() + '\n')
formats = {'.mp3', '.wav', '.flac', '.m4a', '.aac', '.ogg', '.mp4'}
inputs = sorted(p for p in args.source.iterdir() if p.is_file() and p.suffix.lower() in formats)
seen = set()
for src in inputs:
    key = src.stem.casefold()
    if key in seen:
        parser.error(f'duplicate song name: {src.stem}; rename before converting')
    seen.add(key)
    if src.resolve() == (out / (src.stem + '.wav')).resolve():
        parser.error('source and output music folders must be different')
for src in inputs:
    target = out / (src.stem + '.wav')
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', str(src), '-vn',
                    '-ar', '44100', '-ac', '2', '-c:a', 'pcm_s16le', str(target)], check=True)
    art = next((src.with_suffix(e) for e in ['.jpg', '.jpeg', '.png'] if src.with_suffix(e).exists()), None)
    if art:
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', str(art), '-vf',
                        'scale=96:96:force_original_aspect_ratio=increase,crop=96:96',
                        '-frames:v', '1', '-pix_fmt', 'yuvj420p', str(out / (src.stem + '.jpg'))], check=True)
    else:
        # Embedded artwork is optional; do not fail audio conversion if none exists.
        embedded = out / (src.stem + '.jpg')
        result = subprocess.run(['ffmpeg', '-v', 'fatal', '-y', '-i', str(src), '-map', '0:v:0',
                                  '-vf', 'scale=96:96:force_original_aspect_ratio=increase,crop=96:96',
                                  '-frames:v', '1', '-pix_fmt', 'yuvj420p', str(embedded)])
        if result.returncode and embedded.exists():
            embedded.unlink()
    lyrics = src.with_suffix('.txt')
    if lyrics.exists():
        (out / (src.stem + '.txt')).write_text(lyrics.read_text(encoding='utf-8')[:8192], encoding='utf-8')
    print(f'Prepared {src.stem}')
print(f'{len(inputs)} tracks ready in {out}')
