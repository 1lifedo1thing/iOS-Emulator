#!/usr/bin/env python3
"""Compare retained, owner-reviewed actual guest cache session evidence.

Offline only: never launches/controls a guest, copies firmware, or converts host
fixtures into guest acceptance. Input witnesses must come from actual allocated
runs. Image/counter hashes are checked; visual review and live process/resource
observations remain owner evidence, explicitly retained in the result.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import statistics

HEX = re.compile(r'[a-f0-9]{64}')
LINE = re.compile(r'^LightTouch TCG IR cache: hits=(\d+) misses=(\d+) rejected=(\d+) stored=(\d+); frontend IR only, fresh native code$', re.M)
IDENTITY = ('engineSHA256', 'frontendSHA256', 'baseIdentitySHA256',
            'seedStateInventorySHA256', 'workloadSHA256', 'hostBootId')
MAX_LOG = 16 * 1024 * 1024


def require(value, message):
    if not value:
        raise ValueError(message)


def uint(value):
    require(type(value) is int and 0 <= value < 2 ** 64, 'Invalid uint64 witness')
    return value


def verified_bytes(item, maximum):
    require(HEX.fullmatch(item['sha256']) is not None, 'Invalid evidence digest')
    with Path(item['path']).open('rb') as stream:
        data = stream.read(maximum + 1)
    require(len(data) <= maximum, 'Evidence byte bound exceeded')
    require(hashlib.sha256(data).hexdigest() == item['sha256'], 'Evidence hash mismatch')
    return data


def counter_records(data):
    require(len(data) <= MAX_LOG, 'Counter log exceeds limit')
    text = data.decode(errors='strict')
    rows = []
    for raw in text.splitlines(keepends=True):
        if raw.startswith('LightTouch TCG IR cache: hits='):
            require(raw.endswith('\n'), 'Incomplete counter record')
            match = LINE.fullmatch(raw[:-1])
            require(match is not None, 'Malformed counter record')
            rows.append(dict(zip(('hits', 'misses', 'rejected', 'stored'), map(int, match.groups()))))
    for row in rows:
        for value in row.values():
            uint(value)
    for previous, current in zip(rows, rows[1:]):
        require(all(current[key] >= previous[key] for key in current), 'Mixed cache epochs')
    return rows


def validated_trial(trial):
    require(trial['scope'] == 'ACTUAL_IOS_GUEST', 'Actual iOS evidence required')
    require(trial['variant'] in ('off', 'cold', 'reuse'), 'Unknown variant')
    for key in IDENTITY[:-1]:
        require(HEX.fullmatch(trial['identity'][key]) is not None, 'Invalid ' + key)
    require(bool(trial['identity']['hostBootId']), 'Host boot witness missing')
    worker = trial['worker']
    require(uint(worker['pid']) > 0 and uint(worker['startTicks']) > 0, 'Worker identity required')
    require(worker['retired'] is True, 'Worker not retired')
    shutdown = trial['shutdown']
    require(shutdown['confirmed'] is True and shutdown['dirty'] is False and
            type(shutdown['nativeExitCode']) is int and shutdown['nativeExitCode'] == 0 and shutdown['forcedSignals'] == [],
            'Strict clean shutdown required')
    require(trial['resources']['admissionPassed'] is True and
            trial['resources']['continuousGuardPassed'] is True, 'Resource guard failed')
    data = verified_bytes(trial['nativeLog'], MAX_LOG)
    rows = counter_records(data)
    if trial['variant'] == 'off':
        require(not rows and b'LightTouch TCG IR cache: experimental enabled,' not in data,
                'Off session unexpectedly enabled')
    else:
        require(b'LightTouch TCG IR cache: experimental enabled,' in data and rows,
                'Actual enabled/counter witness required')
        if trial['variant'] == 'cold':
            require(uint(trial['cache']['initialEntryCount']) == 0, 'Cold cache not initially empty')
            require(rows[-1]['stored'] > 0, 'Cold run stored no cache IR')
        else:
            require(rows[-1]['hits'] > 0, 'Fresh worker reused no cache IR')
        require(HEX.fullmatch(trial['cache']['afterInventorySHA256']) is not None,
                'Cache inventory digest required')
    actions = {}
    for action in trial['actions']:
        require(action['name'] not in actions, 'Duplicate action label')
        require(action['visualReview']['inspected'] is True, 'Actual capture review required')
        verified_bytes(action['visualReview']['beforeImage'], 64 * 1024 * 1024)
        verified_bytes(action['visualReview']['completeImage'], 64 * 1024 * 1024)
        b, e, t, c = [uint(action[key]) for key in
                      ('inputBeginNS', 'inputEndNS', 'lastTransitionSourceNS', 'firstCompleteSourceNS')]
        require(b <= e <= t <= c, 'Invalid input/source-frame order')
        require(action['visualReview']['scene'] == action['expectedScene'], 'Guest scene mismatch')
        cpu_before, cpu_after = map(uint, (action['workerCpuBeforeNS'], action['workerCpuAfterNS']))
        require(cpu_before <= cpu_after, 'Worker CPU time decreased')
        actions[action['name']] = {'latencyBracketNS': [t - e, c - b],
                                   'workerProcessCpuNS': cpu_after - cpu_before}
    require(actions, 'No actual reviewed guest actions')
    return {'variant': trial['variant'], 'identity': trial['identity'],
            'worker': worker, 'actions': actions,
            'counters': rows[-1] if rows else None, 'source': trial}


def compare(trials):
    rows = [validated_trial(trial) for trial in trials]
    require(rows, 'No trials')
    reference = rows[0]['identity']
    require(all(row['identity'] == reference for row in rows),
            'Engine/frontend/base/seed/workload/host mismatch')
    workers = [(row['worker']['pid'], row['worker']['startTicks']) for row in rows]
    require(len(workers) == len(set(workers)), 'Same-process warming is not persistent reuse')
    groups = {name: [row for row in rows if row['variant'] == name]
              for name in ('off', 'cold', 'reuse')}
    require(all(len(group) >= 2 for group in groups.values()), 'At least two independent runs per variant required')
    for reuse in groups['reuse']:
        require(any(cold['source']['trialId'] == reuse['source']['cache']['producerTrialId'] and
                    cold['source']['cache']['afterInventorySHA256'] == reuse['source']['cache']['beforeInventorySHA256']
                    for cold in groups['cold']), 'Reuse cache not bound to a recorded cold producer')
    names = set(rows[0]['actions'])
    require(all(set(row['actions']) == names for row in rows), 'Action sets differ')
    result = {}
    for name in sorted(names):
        result[name] = {}
        for variant, group in groups.items():
            result[name][variant] = {
                'runs': len(group),
                'medianLatencyBracketNS': [statistics.median(row['actions'][name]['latencyBracketNS'][i]
                                                            for row in group) for i in (0, 1)],
                'medianWorkerProcessCpuNS': statistics.median(row['actions'][name]['workerProcessCpuNS']
                                                             for row in group)}
        off = result[name]['off']['medianLatencyBracketNS']
        reuse = result[name]['reuse']['medianLatencyBracketNS']
        result[name]['medianOffMinusReuseBracketNS'] = [off[0] - reuse[1], off[1] - reuse[0]]
    return {'schema': 1, 'status': 'OWNER_EVIDENCE_COMPARED', 'identity': reference,
            'actions': result, 'trialIds': [row['source']['trialId'] for row in rows],
            'limits': [
                'Owner supplied live identities, scene/source-frame/CPU/resource/shutdown witnesses; not independently observed by this offline tool.',
                'Image hashes establish retained bytes, not the accuracy of visual review.',
                'Timing brackets use observed input/source frames; median differences are observations, not causal attribution.',
                'Control file-cache, guest state, thermal and run order effects in the actual trial procedure.',
                'Worker process CPU includes emulation, rendering/audio/runtime threads; it is not iOS CPU utilization.',
                'Whole-session cache counters and Android milestones are not action-specific final counts.',
                'This report does not qualify MMU/dispatch changes, general workloads or both-port release acceptance.',
            ]}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('manifest', type=Path)
    a = p.parse_args()
    data = a.manifest.read_bytes()
    require(len(data) <= 1024 * 1024, 'Manifest exceeds1MiB')
    manifest = json.loads(data)
    require(manifest['schema'] == 1, 'Unsupported schema')
    result = compare(manifest['trials'])
    result['manifestSHA256'] = hashlib.sha256(data).hexdigest()
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
