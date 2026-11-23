#!/usr/bin/env python3
"""QA gate: still OCR, ffprobe, full decode, loudness, first-frame luma, GIF."""
import json, re, subprocess, sys
from pathlib import Path
from PIL import Image, ImageStat

DIR = Path(__file__).parent
REPO = DIR.parent.parent
FF = '/home/lakshaya/.hermes/tools/ffmpeg-9.0.1-linux-x64/bin/ffmpeg'
FP = '/home/lakshaya/.hermes/tools/ffmpeg-9.0.1-linux-x64/bin/ffprobe'
TIMES = [1, 5, 9, 13, 18, 23, 28]
EXPECT = {
    1: ['compression is a', 'black box'],
    2: ['eight methods', 'no evidence'],
    3: ['kolma measures first'],
    4: ['evidence', 'vibes', '0.5376', '39.30 mb/s', '16bc7172', '11.37 mib', '6.11 mib'],
    5: ['new: rbrc', '0.2590', '57.1 mb/s', '0.2880', '0.3577', '0.7510'],
    6: ['one engine', 'three surfaces'],
    7: ['kolma', 'langersword.github.io/kolma'],
}

def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)

def norm(s):
    s = s.lower().replace('\n', ' ')
    return re.sub(r'\s+', ' ', s)

def ocr(path):
    outs = []
    for psm in ('11', '6'):
        r = run(['tesseract', str(path), 'stdout', '--psm', psm])
        outs.append(norm(r.stdout))
    return ' || '.join(outs)

def still_checks(stage):
    rows = []
    for i, t in enumerate(TIMES, start=1):
        p = DIR / ('previews' if stage == 'previews' else 'qa') / f'{"still" if stage == "previews" else "final"}_{t:02d}.png'
        if not p.exists():
            rows.append({'t': t, 'missing': ['IMAGE MISSING'], 'text': ''})
            continue
        text = ocr(p)
        missing = [k for k in EXPECT[i] if k not in text]
        rows.append({'t': t, 'beat': i, 'missing': missing, 'text': text[:420]})
    return rows

def main():
    stage = sys.argv[1] if len(sys.argv) > 1 else 'final'
    report: dict = {'stage': stage}
    if stage == 'previews':
        report['stills'] = still_checks('previews')
        report['pass'] = all(not r['missing'] for r in report['stills'])
        (DIR / 'qa-previews.json').write_text(json.dumps(report, indent=2))
        print(json.dumps({'pass': report['pass'], 'missing': {r['t']: r['missing'] for r in report['stills'] if r['missing']}}, indent=2))
        return 0 if report['pass'] else 1

    qa = DIR / 'qa'
    qa.mkdir(exist_ok=True)
    mp4 = REPO / 'docs' / 'demo.mp4'
    gif = REPO / 'docs' / 'demo.gif'

    # Extract final frames at beat boundaries.
    for t in TIMES:
        run([FF, '-y', '-v', 'error', '-ss', str(t), '-i', str(mp4), '-frames:v', '1', str(qa / f'final_{t:02d}.png')])
    run([FF, '-y', '-v', 'error', '-i', str(mp4), '-frames:v', '1', str(qa / 'first.png')])

    probe = run([FP, '-v', 'error', '-show_entries', 'format=duration,size,bit_rate',
                 '-show_entries', 'stream=index,codec_type,codec_name,width,height,avg_frame_rate,r_frame_rate,sample_rate,channels',
                 '-of', 'json', str(mp4)])
    report['probe'] = json.loads(probe.stdout)
    dec = run([FF, '-v', 'error', '-i', str(mp4), '-f', 'null', '-'])
    report['decode_errors'] = dec.stderr.strip().splitlines()
    loud = run([FF, '-hide_banner', '-nostats', '-i', str(mp4),
                '-af', 'loudnorm=I=-16:TP=-1.5:LRA=11:print_format=json', '-f', 'null', '-'])
    blocks = re.findall(r'\{\s*"input_i".*?\}', loud.stderr, re.S)
    report['loudness'] = json.loads(blocks[-1]) if blocks else {'raw': loud.stderr[-1200:]}

    first = Image.open(qa / 'first.png').convert('L')
    report['first_frame_luma_mean'] = round(ImageStat.Stat(first).mean[0], 2)
    report['first_frame_luma_max'] = first.getextrema()[1]

    report['stills'] = still_checks('final')
    gi = Image.open(gif)
    report['gif'] = {'size': list(gi.size), 'frames': gi.n_frames, 'bytes': gif.stat().st_size}
    report['files'] = {'mp4_bytes': mp4.stat().st_size, 'gif_bytes': gif.stat().st_size}
    report['pass'] = (not report['decode_errors']
                      and report['first_frame_luma_mean'] < 90
                      and all(not r['missing'] for r in report['stills']))
    (DIR / 'qa-report.json').write_text(json.dumps(report, indent=2))
    print(json.dumps({
        'pass': report['pass'],
        'probe': report['probe'],
        'decode_errors': len(report['decode_errors']),
        'loudness': {k: report['loudness'].get(k) for k in ('input_i', 'input_tp', 'input_lra')},
        'first_frame_luma_mean': report['first_frame_luma_mean'],
        'missing': {r['t']: r['missing'] for r in report['stills'] if r['missing']},
        'gif': report['gif'],
    }, indent=2))
    return 0 if report['pass'] else 1

if __name__ == '__main__':
    sys.exit(main())
