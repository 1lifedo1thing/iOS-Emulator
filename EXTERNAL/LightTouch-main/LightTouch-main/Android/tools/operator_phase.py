"""Finite reviewed guest-navigation budgets measured from one session origin."""

def phase_cutoff_ns(started_ns, phase):
    """Use the original session clock for initial Home or keyboard preparation.

    Both phases may need one balanced Home/unlock after observed guest sleep.
    Keyboard preparation reserves the original 180 seconds for media/cleanup;
    selecting a later phase never creates a new session or wake deadline.
    """
    if type(started_ns) is not int or started_ns <= 0:
        raise ValueError('original monotonic session origin required')
    if phase not in ('awake', 'keyboard'):
        raise ValueError('Lock input is limited to reviewed awake/keyboard phases')
    seconds = 180 if phase == 'awake' else 765 - 180
    return started_ns + seconds * 1_000_000_000

