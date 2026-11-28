# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""subprocess.run for a test's driver that's fed input: the input goes through a temporary file, never a pipe.

On macOS, poll() reports a pipe writable with less than PIPE_BUF (512) bytes free, and CPython's communicate()
then makes a blocking 512-byte write of the input into it (Popen._communicate's write loop on POSIX, seen in 3.9
through 3.15). A driver that answers each line meanwhile fills its 16 KB stdout pipe and waits for it to be read,
while the parent waits in that write: both at 0 % CPU, forever (communicate's timeout can't fire inside a write).
Input past macOS's 16 KB pipe is enough, and when the kernel's pipe-buffer pool runs low (a loaded Mac), new pipes
hold only 512 bytes, so far smaller inputs hang too; Linux's 64 KB pipes hide it. From a file the driver reads its
input whenever it wants, and only the output comes through pipes, which communicate() drains. Every test that feeds
a driver input uses this.
"""

import locale
import subprocess
import tempfile

# A driver that hangs anyway fails its test instead of the run. (Under tools/stuck_watch.py, as in CI, a module
# capped at this long or less is stopped first, and reported STUCK with what it waited on.)
DEFAULT_TIMEOUT = 900


def run_child(cmd, input=None, text=None, timeout=DEFAULT_TIMEOUT, **kw):
    if input is None:
        return subprocess.run(cmd, text=text, timeout=timeout, **kw)
    with tempfile.TemporaryFile() as f:
        # str input in the caller's encoding, else the one subprocess.run's text mode would use
        f.write(input.encode(kw.get('encoding') or locale.getpreferredencoding(False), kw.get('errors') or 'strict')
                if isinstance(input, str) else input)
        f.seek(0)
        return subprocess.run(cmd, stdin=f, text=text, timeout=timeout, **kw)
