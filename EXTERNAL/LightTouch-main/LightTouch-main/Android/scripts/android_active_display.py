"""Check effective Android input geometry without changing device settings.

The accelerometer_rotation preference controls automatic rotation; it does not
report the current display orientation. Read `adb shell dumpsys input` for the
active viewport instead. Duplicate InputReader sections must agree.
"""


def active_display_matches(raw, *, width, height, density_dpi, orientation=0, display_id=0):
    """Return whether every active internal viewport for the display matches.

    This is a passive configuration check, not guest rendering/input evidence.
    The caller supplies its expected geometry and retains the original output.
    Missing, malformed, inactive or conflicting viewport data fail closed.
    """
    values = (width, height, density_dpi, orientation, display_id)
    if any(type(value) is not int for value in values):
        return False
    if min(width, height, density_dpi) <= 0 or orientation not in range(4) or display_id < 0:
        return False
    try:
        text = raw.decode('utf-8') if isinstance(raw, bytes) else raw
        if not isinstance(text, str):
            return False
    except UnicodeDecodeError:
        return False
    active = [line.strip() for line in text.splitlines()
              if 'Viewport INTERNAL:' in line
              and 'displayId=%d,' % display_id in line and 'isActive=[1]' in line]
    expected = ['orientation=%d,' % orientation, 'densityDpi=%d ' % density_dpi,
                'logicalFrame=[0, 0, %d, %d]' % (width, height),
                'physicalFrame=[0, 0, %d, %d]' % (width, height),
                'deviceSize=[%d, %d]' % (width, height)]
    return bool(active) and all(all(token in line for token in expected) for line in active)
