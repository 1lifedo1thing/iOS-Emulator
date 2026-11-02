"""Validate passive pre-native worker status and admitted trace setup receipts.

Accepting a trace-arm request does not configure native tracing or prove guest
input delivery. These helpers inspect retained evidence and never contact ADB.
"""


def unused_worker_matches(status, *, pid, uid, graphics_provider=None):
    """Check exact identity and unused, clean pre-native-runtime status."""
    if type(pid) is not int or type(uid) is not int or min(pid, uid) <= 0:
        return False
    if graphics_provider is not None and (not isinstance(graphics_provider, str) or not graphics_provider):
        return False
    if not isinstance(status, dict):
        return False
    observations = status.get('observations')
    if not isinstance(observations, dict):
        return False
    if (type(status.get('pid')) is not int or type(status.get('uid')) is not int
            or status['pid'] != pid or status['uid'] != uid
            or status.get('state') != 'stopped' or status.get('error') != ''
            or type(observations.get('producedFrames')) is not int
            or observations['producedFrames'] != 0):
        return False
    for key in ('sessionStarted', 'nativePollSnapshotValid', 'nativeStateDirty',
                'nativeStorageFailed', 'touchTraceConfigured'):
        if status.get(key) is not False:
            return False
    return (graphics_provider is None or
            (status.get('graphicsProviderChoice') == graphics_provider
             and status.get('graphicsProviderFixed') is True))


def pre_native_trace_arm_admitted(receipt):
    """Check request admission without promoting it to runtime/delivery proof.

    Identity, worker status, original receipt hash and actual guest acceptance
    must be checked separately by the caller.
    """
    return (isinstance(receipt, dict)
            and receipt.get('touchTraceArmAdmissionAccepted') is True
            and receipt.get('touchTraceArmError') == ''
            and receipt.get('touchTraceConfiguredAfterArm') is False
            and receipt.get('traceDeliveryVerified') is False)
