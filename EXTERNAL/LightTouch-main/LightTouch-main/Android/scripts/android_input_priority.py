"""Service finite reviewed input before optional observation work.

This helper authorizes no input, retries nothing and owns no deadlines. The
caller validates its worker, immutable reviewed captures and original bounds.
Call checkpoints between blocking reads, and abandon an in-progress capture
when a checkpoint returns True: an action invalidates that capture's context.
"""
import re


def request_paths(directory):
    """Completion/dispatch receipts cannot enter the UUID command namespace."""
    return [path for path in sorted(directory.glob('action-*.json')) if re.fullmatch(
        r'action-[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\.json', path.name)]


def service_pending(directory, seen, dispatch, completed, maximum=12):
    """Dispatch pending commands exactly once, before starting optional work.

    The callback retains all admission checks. Mark a request seen before its
    callback: uncertain or failed input is never replayed. Exceptions propagate
    to the controller's ordinary Stop path. Completion may invalidate captures.
    """
    serviced = False
    for path in request_paths(directory):
        if path.name in seen:
            continue
        if len(seen) >= maximum:
            raise ValueError('Original finite input limit exhausted')
        seen.add(path.name)
        result = dispatch(path)
        completed(path, result)
        serviced = True
    return serviced
