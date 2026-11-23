#!/usr/bin/env bash
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
FF=/home/lakshaya/.hermes/tools/ffmpeg-9.0.1-linux-x64/bin/ffmpeg
RAW="$(node -p "require('$DIR/record.json').rawPath")"

# Cut the Playwright pre-roll, lock to exactly 30 s / 30 fps, mux the synth bed.
"$FF" -y -ss 0.30 -i "$RAW" -i "$DIR/bed.wav" -t 30 \
  -filter_complex "[1:a]atrim=0:30,afade=t=in:st=0:d=0.45,afade=t=out:st=28.55:d=1.45,volume=0.82,loudnorm=I=-16:TP=-1.5:LRA=11[a]" \
  -map 0:v:0 -map "[a]" -vf fps=30 -c:v libx264 -crf 18 -preset medium \
  -pix_fmt yuv420p -movflags +faststart -c:a aac -b:a 192k "$REPO/docs/demo.mp4"

# Repo already shipped a GIF preview; regenerate it from the new cut.
"$FF" -y -i "$REPO/docs/demo.mp4" \
  -vf "fps=8,scale=720:-2:flags=lanczos,split[s0][s1];[s0]palettegen=max_colors=64[p];[s1][p]paletteuse=dither=bayer:bayer_scale=2" \
  -loop 0 "$REPO/docs/demo.gif"

echo "mp4: $REPO/docs/demo.mp4"
echo "gif: $REPO/docs/demo.gif"
