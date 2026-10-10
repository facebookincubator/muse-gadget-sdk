# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Compile the real board-owned descriptors into host protocol fixtures."""
from pathlib import Path


def prepare_board_matrix(directory, root):
    source = (root / "components/muse/boards/board_sensecap_watcher.c").read_text()
    start = source.index("/* Board-owned descriptors:")
    end = source.index("/* End board-owned power-test descriptors. */", start)
    block = source[start:end]
    # The fake's hook dispatch can also select a deliberately different board.
    block = block.replace("muse_ptest_board_name", "reference_board_name")
    block = block.replace("muse_ptest_board_matrix", "reference_board_matrix")
    (Path(directory) / "power_test_board_matrix.inc").write_text(block)
