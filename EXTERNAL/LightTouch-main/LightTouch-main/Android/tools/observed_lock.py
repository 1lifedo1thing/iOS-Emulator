"""Host-side continuity against a personally inspected guest Lock screenshot.

This never recognizes a new scene or supplies a guest acceptance verdict.
Pillow is needed only for the Surface anchor comparison.
"""
import hashlib
import math
from pathlib import Path
import re


def producer_frames(texts):
    matches = [int(value) for text in texts
               for value in re.findall(r'\bframes ([0-9]+)\b', text)]
    if len(matches) != 1:
        raise ValueError('one actual UI producer-frame counter required')
    return matches[0]


def fit(view):
    if len(view) != 4 or any(type(value) is not int for value in view):
        raise ValueError('actual integer Surface bounds required')
    left, top, right, bottom = view
    if min(left, top) < 0 or right <= left or bottom <= top:
        raise ValueError('invalid Surface bounds')
    scale = min((right - left) / 320, (bottom - top) / 480)
    return (left + ((right - left) - 320 * scale) / 2,
            top + ((bottom - top) - 480 * scale) / 2, scale)


def compare_surface(reference, current, view, policy):
    """Compare the existing policy's stable regions in unresampled originals."""
    from PIL import Image
    left, top, scale = fit(view)
    results = []
    for path in (Path(reference), Path(current)):
        if not 0 < path.stat().st_size <= 8 * 1024 * 1024:
            raise ValueError('bounded original PNG required')
    with Image.open(reference) as old, Image.open(current) as new:
        if (old.format != 'PNG' or new.format != 'PNG' or old.size != new.size or
                max(old.size) > 4096 or view[2] > old.width or view[3] > old.height):
            raise ValueError('original PNG shape or actual Surface changed')
        for name, (x1, y1, x2, y2) in policy.REGIONS:
            box = (math.ceil(left + x1 * scale), math.ceil(top + y1 * scale),
                   math.floor(left + x2 * scale), math.floor(top + y2 * scale))
            if box[2] <= box[0] or box[3] <= box[1]:
                raise ValueError('anchor is empty at actual Surface scale')
            first = old.crop(box).convert('RGBA').tobytes()
            second = new.crop(box).convert('RGBA').tobytes()
            results.append(dict(name=name, nativeBounds=[x1, y1, x2, y2],
                                actualSurfaceBounds=box, exactMatch=first == second,
                                referenceRgbaSha256=hashlib.sha256(first).hexdigest(),
                                currentRgbaSha256=hashlib.sha256(second).hexdigest()))
    return dict(policy=policy.POLICY, matched=all(x['exactMatch'] for x in results),
                anchors=results, excludedDynamic=policy.EXCLUDED_DYNAMIC,
                noNewSceneAsserted=True, guestAcceptance=False,
                comparison='exact original Surface anchor pixels; no resampling or tolerance')


class WakeReadiness:
    """Bounded host observation, separate from native/frame atomicity claims."""
    def __init__(self, began_ns, original_cutoff_ns, baseline_frames, identity,
                 reference_view, reference_rotation):
        if (type(began_ns) is not int or type(original_cutoff_ns) is not int or
                original_cutoff_ns <= began_ns or type(baseline_frames) is not int or
                baseline_frames < 0):
            raise ValueError('original wake deadline and actual baseline required')
        fit(reference_view)
        if reference_rotation not in range(4):
            raise ValueError('actual display rotation required')
        self.deadline_ns = min(original_cutoff_ns, began_ns + 15_000_000_000)
        self.baseline_frames = baseline_frames
        self.identity = identity
        self.view = list(reference_view)
        self.rotation = reference_rotation

    def observe(self, capture, now_ns):
        """Require fresh current UI frame growth after Home and matching anchors."""
        if now_ns > self.deadline_ns:
            raise ValueError('original wake observation deadline expired')
        if capture['identity'] != self.identity:
            raise ValueError('current worker identity changed')
        if (capture['rotation'] != self.rotation or capture['view'] != self.view or
                capture['state'] != 'running' or capture['sleeping']):
            return False
        if not 0 <= now_ns - capture['captureEndNs'] <= 5_000_000_000:
            raise ValueError('actual current capture is stale or future dated')
        frames = capture['producedFrames']
        if type(frames) is not int or frames < self.baseline_frames:
            raise ValueError('missing or regressing actual UI producer counter')
        return frames > self.baseline_frames and capture['anchorsMatched'] is True
