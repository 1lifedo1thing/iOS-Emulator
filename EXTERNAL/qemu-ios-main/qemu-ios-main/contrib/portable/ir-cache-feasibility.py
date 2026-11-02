#!/usr/bin/env python3
"""Bounded offline cache observations, including failed runs; never acceptance."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import statistics

spec = importlib.util.spec_from_file_location('cache_session_report',
    Path(__file__).with_name('ir-cache-session-report.py'))
session = importlib.util.module_from_spec(spec)
spec.loader.exec_module(session)

MAX_RECORDS = 4096
MAX_BYTES = 32 * 1024**2
ENABLE = re.compile(r'^LightTouch TCG IR cache: experimental enabled, mapped-image-sha256=([a-f0-9]{64}); 32MiB/4096 records maximum$', re.M)


def load(path, limit):
    with Path(path).open('rb') as stream:
        data = stream.read(limit + 1)
    session.require(len(data) <= limit, 'Input exceeds bounded offline read')
    return data


def ratio(numerator, denominator):
    return numerator / denominator if denominator else None


def observe(native_log, cache_receipt, runtime_result):
    data = load(native_log, session.MAX_LOG)
    receipt_bytes = load(cache_receipt, 4 * 1024**2)
    result_bytes = load(runtime_result, 1024**2)
    receipt, result = json.loads(receipt_bytes), json.loads(result_bytes)
    session.require(hashlib.sha256(receipt_bytes).hexdigest() == result['cache_evidence_sha256'],
                    'Runtime result does not bind supplied cache receipt')
    samples = session.counter_records(data)
    mode = receipt['mode']
    session.require(mode in ('off', 'cold', 'reuse'), 'Unknown cache mode')
    enabled = ENABLE.findall(data.decode())
    if mode == 'off':
        session.require(not samples and not enabled, 'OFF emitted native cache telemetry')
        statistics_data = None
    else:
        session.require(len(enabled) == 1 and samples, 'One actual enable and native samples required')
        after, before = receipt['after'], receipt['before']
        session.require(after['mappedImageSHA256'] == enabled[0], 'Mapped engine mismatch')
        session.require(samples[-1] == receipt['native_final'], 'Native final/receipt mismatch')
        records = session.uint(after['records']); total_bytes = session.uint(after['bytes'])
        session.require(records <= MAX_RECORDS and total_bytes <= MAX_BYTES, 'Cache exceeds format limits')
        entries = after['entries']
        sizes = [session.uint(entry['bytes']) for entry in entries.values()]
        session.require(len(entries) == records and sum(sizes) == total_bytes,
                        'Cache inventory does not match aggregate')
        session.require(all(104 < n <= 104 + 128 * 1024 for n in sizes), 'Invalid record size')
        final = samples[-1]
        initial = session.uint(before['records'])
        session.require(final['stored'] == records - initial, 'Stores/inventory delta mismatch')
        session.require(mode != 'cold' or initial == 0, 'COLD was not empty')
        eligible_lookups = sum(final[key] for key in ('hits', 'misses', 'rejected'))
        saturation = next((row for row in samples if initial + row['stored'] == MAX_RECORDS), None)
        statistics_data = {
            'nativeFinal': final,
            'candidateLookupAttempts': eligible_lookups,
            'candidateLookupHitFraction': ratio(final['hits'], eligible_lookups),
            'translatedArmTbAttempts': None,
            'a32KeyEligibleFractionOfAllTranslatedTbs': None,
            'translatedThumbTbAttempts': None,
            'candidateCaptureRejects': None,
            'records': records, 'bytes': total_bytes,
            'recordCapacityFraction': records / MAX_RECORDS,
            'byteCapacityFraction': total_bytes / MAX_BYTES,
            'recordSizeBytes': {'minimum': min(sizes) if sizes else None,
                'median': statistics.median(sizes) if sizes else None,
                'maximum': max(sizes) if sizes else None},
            'firstReportedRecordCountSaturation': saturation,
            'coldStoreYieldAtCountSaturation': (ratio(saturation['stored'],
                sum(saturation[key] for key in ('hits', 'misses', 'rejected')))
                if saturation and mode == 'cold' else None),
            'fsyncCallLowerBoundFromSuccessfulPublishes': 2 * final['stored'],
            'measuredFileFsyncWallTimeNS': None,
            'measuredDirectoryFsyncWallTimeNS': None,
            'measuredSaturationMonotonicNS': None,
            'measuredFrontendDecodeCpuTimeNS': None,
            'measuredIrOptimizationAndEmissionCpuTimeNS': None,
        }
    evidence = {name: {'path': str(path), 'sha256': hashlib.sha256(raw).hexdigest()}
                for name, path, raw in [('nativeLog', native_log, data),
                    ('cacheReceipt', cache_receipt, receipt_bytes),
                    ('runtimeResult', runtime_result, result_bytes)]}
    return {'schema': 1, 'status': 'OFFLINE_OBSERVATIONS_ONLY', 'mode': mode,
        'sourceRuntimeStatus': result['status'],
        'sourceConfirmedClean': result.get('confirmed_clean_receipt'),
        'sourceDirty': result.get('dirty'), 'evidence': evidence,
        'statistics': statistics_data, 'guestQualified': False,
        'androidQualified': False, 'performanceQualified': False,
        'limits': [
            'Failed and dirty runs are retained observations, never repaired or promoted by this tool.',
            'Candidate lookup denominator includes hits, absent keys and found-but-rejected records; it excludes ineligible Thumb/CPU/context TBs.',
            'Translation attempts count generated TBs, not dynamic instruction executions or guest CPU utilization.',
            'Store yield is not A32 eligibility, unique-code coverage, execution-weighted coverage or a predicted speedup.',
            'Existing native milestones contain no per-event timestamps or frontend/optimization/emission/fsync durations; null means unmeasured.',
            'Each successful record publication calls file fsync and directory fsync; extra failed attempts and sync latency are unknown.',
            'Current cache covers a closed A32 ARM1176 instruction/IR subset; helpers, Thumb, VFP/SIMD and other CPUs are excluded.',
            'Every hit still validates/decodes symbolic IR, optimizes it and emits fresh native code; repeated execution already uses in-memory TBs.',
            'A speedup requires measured frontend CPU share and workload-weighted hit coverage to exceed lookup/replay/store costs.',
        ]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-log', required=True, type=Path)
    parser.add_argument('--cache-receipt', required=True, type=Path)
    parser.add_argument('--runtime-result', required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(observe(args.native_log, args.cache_receipt, args.runtime_result), indent=2))


if __name__ == '__main__':
    main()
