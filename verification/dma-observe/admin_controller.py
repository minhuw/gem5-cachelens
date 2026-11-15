#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""One bounded Admin ON case; parent owns the managed process EXIT watch."""
import datetime
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path('/home/minhuw/cachelens/.pi/workspaces/admin-native-same-run-v1')
HERE = ROOT / '.pi/run-on'
RECEIPT = HERE / 'receipt.json'
PLAN = json.loads((HERE / 'plan.json').read_text())
START_UTC = datetime.datetime.now(datetime.timezone.utc)
START_MONO = time.monotonic_ns()
BOUND = 600
state = dict(schema='cachelens.independent-admin-on-controller-v1', status='ON_STARTED',
    controller_pid=os.getpid(), start_utc=START_UTC.isoformat(),
    start_monotonic_ns=START_MONO, wall_bound_seconds=BOUND,
    deadline_utc=(START_UTC + datetime.timedelta(seconds=BOUND)).isoformat(),
    deadline_monotonic_ns=START_MONO + BOUND * 10**9,
    command=PLAN['argv'], container_name=PLAN['container_name'],
    source_freeze=str(HERE / 'source-freeze.json'),
    source_commit=PLAN['source_commit'], compiled_commit_claim=False,
    build_receipt=str(ROOT / '.pi/build/attempt1/receipt.json'),
    native_sha256=PLAN['native_sha256'], runner_sha256=PLAN['runner_sha256'],
    output=PLAN['output'], child_state=str(HERE / 'children.json'),
    case_log=str(HERE / 'case.log'), on_case_exit=None, off_executed=False,
    analysis_policy='Exactly once in original admin_run.py after this ON exits; no second analyzer',
    parent_action='Register direct process_wait until EXIT for the returned managed ID; inspect this original receipt and case manifest, then resume same owner. No automatic OFF.')


def save():
    managed = HERE / 'managed-start.json'
    if managed.exists():
        state['managed'] = json.loads(managed.read_text())
    temporary = HERE / 'receipt.tmp'
    temporary.write_text(json.dumps(state, indent=2) + '\n')
    temporary.replace(RECEIPT)


child = None
try:
    if RECEIPT.exists():
        raise RuntimeError('ON receipt already exists; one attempt only, no overwrite')
    save()
    print('ADMIN_ON_STARTED controller=%d start_utc=%s start_mono=%d deadline=%s receipt=%s' %
          (os.getpid(), state['start_utc'], START_MONO, state['deadline_utc'], RECEIPT), flush=True)
    with (HERE / 'case.log').open('xb') as log:
        child = subprocess.Popen(PLAN['argv'], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
        state['docker_cli_pid'] = child.pid
        save()
        remaining = max(.01, (state['deadline_monotonic_ns'] - time.monotonic_ns()) / 1e9)
        try:
            code = child.wait(timeout=remaining)
        except subprocess.TimeoutExpired:
            state['timed_out'] = True
            # Signals only the named container created by this one controller.
            subprocess.run(['docker', 'rm', '--force', PLAN['container_name']],
                           stdout=log, stderr=subprocess.STDOUT, timeout=20)
            child.kill()
            child.wait(timeout=5)
            raise RuntimeError('Admin ON exceeded explicit600s wall bound; no retry or OFF')
    state['on_case_exit'] = code
    children = HERE / 'children.json'
    if children.exists():
        state['children'] = json.loads(children.read_text())
    manifest_path = Path(PLAN['output']) / 'manifest.json'
    if manifest_path.exists():
        manifest = json.loads(manifest_path.read_text())
        state.update(manifest=str(manifest_path), passed=manifest['passed'],
                     analysis_invocations=manifest['analysis_invocations'])
    if code:
        raise RuntimeError('Admin ON exit ' + str(code) + '; inspect original case.log/manifest; no retry or OFF')
    if not state.get('passed') or state.get('analysis_invocations') != 1:
        raise RuntimeError('Admin ON lacks one actual successful completion/ledger/control analysis')
    state['status'] = 'ON_PASS_AWAIT_PARENT_REVIEW_NO_OFF'
    state['finish_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    save()
    print('ADMIN_ON_PASS_AWAIT_PARENT_REVIEW receipt=' + str(RECEIPT), flush=True)
except BaseException as exc:
    state['status'] = 'ON_FAILED_NO_OFF'
    state['failure'] = dict(type=type(exc).__name__, detail=str(exc))
    state['finish_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    save()
    print('ADMIN_ON_FAILED: ' + str(exc), file=sys.stderr, flush=True)
    sys.exit(1)
