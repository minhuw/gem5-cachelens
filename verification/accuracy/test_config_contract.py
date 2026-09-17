"""Host-only checks of the actual config's memory override; no gem5 binary."""
import ast
from pathlib import Path
from types import SimpleNamespace

CONFIG = Path(__file__).resolve().parents[2] / "configs/simbricks/i40e_accuracy.py"
tree = ast.parse(CONFIG.read_text())


class Range:
    def __init__(self, start, size=None):
        self.start, self.length = (0, start) if size is None else (start, size)

    def size(self):
        return self.length


# Execute only the configuration constants and isolated memory method. Do not
# mock gem5 imports or pretend this exercises native ports/protocols.
constants = [node for node in tree.body if isinstance(node, ast.Assign)]
board_class = next(node for node in tree.body if isinstance(node, ast.ClassDef))
method = next(node for node in board_class.body if isinstance(node, ast.FunctionDef)
              and node.name == "_setup_memory_ranges")
namespace = {"AddrRange": Range}
exec(compile(ast.Module(body=constants + [method], type_ignores=[]), str(CONFIG), "exec"), namespace)
gib = namespace["GIB"]
geometry = namespace["GEOMETRY"]
assert geometry["hnf_size"] == "2560KiB" and geometry["hnf_assoc"] == 20
assert geometry["num_hnfs"] == 8 and geometry["ddio_way_part"] == 2
assert geometry["l1i_size"] == geometry["l1d_size"] == "32KiB"
assert geometry["l2_size"] == "256KiB"
assert geometry["l1i_assoc"] == geometry["l1d_assoc"] == geometry["l2_assoc"] == 8
assert geometry["model_profile"] == "intel-ddio"
assert geometry["hnf_inclusion"] == "inclusive"
assert geometry["indexing_policy"] == "splitmix64"
assert geometry["core_clock"] == "2GHz"

for size, ruby in ((16 * gib, True), (8 * gib, True), (17 * gib, True), (16 * gib, False)):
    ranges = []
    memory = SimpleNamespace(get_size=lambda: size, set_memory_range=ranges.extend)
    board = SimpleNamespace(get_memory=lambda: memory,
                            get_cache_hierarchy=lambda: SimpleNamespace(is_ruby=lambda: ruby))
    try:
        namespace["_setup_memory_ranges"](board)
    except ValueError:
        assert size != 16 * gib or not ruby
        continue
    assert size == 16 * gib and ruby
    spans = [(r.start, r.start + r.size()) for r in ranges]
    assert spans == [(0, 3 * gib), (4 * gib, 17 * gib)]
    assert sum(r.size() for r in ranges) == 16 * gib
    assert board.main_mem_ranges == board._ram_ranges == ranges
    assert len(board.mem_ranges) == 3
    assert board.mem_ranges[-1].start == 0xC0000000
    assert board.mem_ranges[-1].size() == 0x100000
    for lo, hi in [(0xC0000000, 0xC0100000), (17 * gib, 33 * gib)]:
        assert all(hi <= start or lo >= end for start, end in spans)
print("ACCURACY_HOST_CONFIG_CONTRACT_OK (not native instantiation)")
