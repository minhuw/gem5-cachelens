# Copyright (c) 2026 minhuw
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

import re
import subprocess
from pathlib import Path

from testlib import *


_CONFIG = joinpath(
    config.base_dir,
    "tests",
    "gem5",
    "mesi_ddio",
    "configs",
    "mesi_ddio_directed.py",
)
_CAPABILITY_ERROR = (
    "rejected a masked DMA write because supports_masked_writes is false"
)


def _run_mi_masked_rejection(params):
    tempdir = Path(params.fixtures[constants.tempdir_fixture_name].path)
    output_dir = tempdir / "mi-masked-rejection"
    gem5 = params.fixtures[constants.gem5_binary_fixture_name].path
    command = [
        gem5,
        "-d",
        output_dir.as_posix(),
        "--debug-flags=RubyDma",
        _CONFIG,
        "--scenario=masked_rejection",
        "--abs-max-tick=1000000",
    ]
    result = subprocess.run(
        command,
        cwd=config.base_dir,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=120,
    )
    print(result.stdout)

    assert result.returncode != 0, "MI_example accepted a masked DMA write"
    assert _CAPABILITY_ERROR in result.stdout, (
        "MI_example failed without the DMASequencer capability error"
    )
    assert "DMA req created" not in result.stdout, (
        "MI_example consumed the masked request before rejecting it"
    )

    config_text = (output_dir / constants.gem5_simulation_config_ini).read_text(
        encoding="utf-8"
    )
    sections = re.findall(
        r"(?ms)^\[([^]]+)\]\n(.*?)(?=^\[|\Z)", config_text
    )
    dma_sections = {
        name: body
        for name, body in sections
        if re.search(r"(?m)^type=DMASequencer$", body)
    }
    assert dma_sections, "MI_example config has no DMASequencer"
    for name, body in dma_sections.items():
        assert re.search(
            r"(?m)^supports_masked_writes=false$", body
        ), f"MI_example DMASequencer {name} did not retain the false default"


for host in constants.supported_hosts:
    name = f"ruby-dma-masked-write-rejected-MI_example-X86-{host}-opt"
    TestSuite(
        name=name,
        fixtures=(
            Gem5Fixture(
                constants.x86_tag,
                constants.opt_tag,
                protocol="MI_example",
            ),
            TempdirFixture(),
        ),
        tests=(TestFunction(_run_mi_masked_rejection, name=name),),
        tags=(
            constants.x86_tag,
            constants.opt_tag,
            constants.quick_tag,
            host,
        ),
    )
