"""Isolated timing prototype v1; integer nanoseconds, not physical calibration."""

def add_timing_args(parser):
    for name in ('pci-latency-ns', 'eth-latency-ns', 'sync-interval-ns'):
        parser.add_argument('--' + name, type=int, default=500)


def validate_timing(a):
    values = (a.pci_latency_ns, a.eth_latency_ns, a.sync_interval_ns)
    if any(type(v) is not int or v <= 0 or v > (2**64 - 1) // 1000 for v in values):
        raise ValueError('latencies/interval must be positive integer ns representable in uint64 ps')
    if a.sync_interval_ns > min(a.pci_latency_ns, a.eth_latency_ns):
        raise ValueError('sync interval must not exceed either adapter latency')


def nic_urls(s, a):
    validate_timing(a)
    return [f'listen:{s}/{channel}.sock:{s}/nic.shm:sync=true:latency={latency * 1000}:sync_interval={a.sync_interval_ns * 1000}'
            for channel, latency in (('pci', a.pci_latency_ns), ('eth', a.eth_latency_ns))]


def pci_url(s, a):
    validate_timing(a)
    return f'connect:{s}/pci.sock:sync:latency={a.pci_latency_ns}ns:sync_interval={a.sync_interval_ns}ns'


def ethernet_params(socket, a):
    validate_timing(a)
    return dict(uxsocket_path=str(socket), shm_path='', listen=False, sync=True,
                link_latency=f'{a.eth_latency_ns}ns', sync_tx_interval=f'{a.sync_interval_ns}ns')


def gem_timing_args(s, a):
    return ['--simbricks-pci', pci_url(s, a), '--i40e-network-socket', str(s / 'eth.sock'),
            '--pci-latency-ns', str(a.pci_latency_ns), '--eth-latency-ns', str(a.eth_latency_ns),
            '--sync-interval-ns', str(a.sync_interval_ns)]


def guest_cpu_args():
    return ['--cpu-type=TimingSimpleCPU', '--num-cpus=8', '--mem-size=16GiB', '--mem-type=SimpleMemory']
