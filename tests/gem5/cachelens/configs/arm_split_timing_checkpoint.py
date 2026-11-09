# SPDX-License-Identifier: BSD-3-Clause
"""Object-level wire checkpoint fixtures after the directed guest has parked.
This deliberately loads only GIC/timer objects in a stopped native simulation;
it is not a full undrained-system checkpoint protocol.
"""
import configparser
import json
from pathlib import Path
import runpy
import sys
import m5
from _m5 import core

CONFIG = Path(__file__).with_name('arm_split_timing.py')
sys.argv = [str(CONFIG), '--mode', 'native', '--partition-timing']
namespace = runpy.run_path(str(CONFIG), run_name='__m5_main__')
board = namespace['board']
out = Path(m5.options.outdir)
original = out / 'original-checkpoint'
m5.checkpoint(str(original))
start = m5.curTick()


def read(directory):
    config = configparser.ConfigParser(interpolation=None)
    config.optionxform = str
    config.read(directory / 'm5.cpt')
    return config


def write(config, directory):
    directory.mkdir()
    with (directory / 'm5.cpt').open('w') as stream:
        config.write(stream)


def capture(directory):
    directory.mkdir()
    core.serializeAll(str(directory))
    return read(directory)


def compare_gic(a, b):
    keys = [key for key in a.sections() if key == gic or key.startswith(gic + '.')]
    assert set(keys) == {key for key in b.sections() if key == gic or key.startswith(gic + '.')}
    for key in keys:
        assert dict(a[key]) == dict(b[key]), (key, dict(a[key]), dict(b[key]))


gic = board.realview.gic.path()
timer = board.realview.generic_timer.path()
fixture = read(original)
generations = fixture[gic]['wireGeneration'].split()
generations[0], generations[3] = '5', '7'
fixture[gic]['wireGeneration'] = ' '.join(generations)
fixture[gic]['wire_delivery_count'] = '4'
records = [dict(cpu=0, fiq='false', post='true', when=start + 1_000_000, generation=4),
           dict(cpu=0, fiq='false', post='false', when=start + 1_001_000, generation=5),
           dict(cpu=0, fiq='false', post='true', when=start + 1_002_000, generation=5),
           dict(cpu=1, fiq='true', post='true', when=start + 1_003_000, generation=7)]
for index, record in enumerate(records):
    fixture[gic + f'.wireDelivery{index}'] = {key: str(value) for key, value in record.items()}
limits = {}
for section in fixture.sections():
    if section.startswith(timer + '.') and section.endswith('.virt_el1_timer'):
        limits[section] = str(0x123456789 + len(limits))
        fixture[section]['_counterLimit'] = limits[section]
        assert fixture[section]['control_serial'] in ('0', '4')
source = out / 'wire-fixture'
write(fixture, source)
checkpoint = core.getCheckpoint(str(source))
board.realview.gic.loadState(checkpoint)
board.realview.generic_timer.loadState(checkpoint)
roundtrip = capture(out / 'wire-roundtrip')
compare_gic(fixture, roundtrip)
for section, limit in limits.items():
    assert roundtrip[section]['_counterLimit'] == limit
print('ARM_TIMING_CHECKPOINT_PASS wire_records=4 timer_limits=4', flush=True)
# Resume and then drain: every restored post must be accounted for until delivery.
m5.simulate(1)
m5.drain()
assert m5.curTick() >= records[-1]['when']
m5.memWriteback(namespace['root'])
landed = capture(out / 'wire-landed')
assert landed[gic]['wire_delivery_count'] == '0'

# Old checkpoints had interrupt_time (and sometimes fiq_time), not wire records.
legacy = read(original)
del legacy[gic]['wire_delivery_count']
del legacy[gic]['wireGeneration']
irq_time = ['0'] * 256
fiq_time = ['0'] * 256
irq_time[0] = str(m5.curTick() + 1_000_000)
fiq_time[1] = str(m5.curTick() + 1_001_000)
legacy[gic]['interrupt_time'] = ' '.join(irq_time)
legacy[gic]['fiq_time'] = ' '.join(fiq_time)
source = out / 'legacy-fixture'
write(legacy, source)
board.realview.gic.loadState(core.getCheckpoint(str(source)))
converted = capture(out / 'legacy-converted')
assert converted[gic]['wire_delivery_count'] == '2'
assert converted[gic + '.wireDelivery0']['when'] == irq_time[0]
assert converted[gic + '.wireDelivery0']['fiq'] == 'false'
assert converted[gic + '.wireDelivery1']['when'] == fiq_time[1]
assert converted[gic + '.wireDelivery1']['fiq'] == 'true'
m5.simulate(1)
m5.drain()
assert m5.curTick() >= int(fiq_time[1])
m5.memWriteback(namespace['root'])
assert capture(out / 'legacy-landed')[gic]['wire_delivery_count'] == '0'
(out / 'checkpoint-checks.json').write_text(json.dumps(dict(
    status='passed', wire_records=records, restored_timer_limits=limits,
    legacy_irq=irq_time[0], legacy_fiq=fiq_time[1], drained_tick=m5.curTick()), indent=2) + '\n')
print('ARM_TIMING_CHECKPOINT_PASS legacy_posts=2 drained=1', flush=True)
