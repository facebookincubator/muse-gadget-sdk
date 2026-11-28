#!/usr/bin/env python3
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
"""mg_rng's health test cutoffs for a measured min-entropy per raw sample (SP 800-90B 4.4, alpha = 2^-20):

    cutoffs.py H [H ...]      H in bits per raw sample (ea_non_iid's, or the restart test's if lower)

prints h_milli, rct_cutoff (1 + ceil(20 / H)) and apt_cutoff (1 + CRITBINOM(512, 2^-H, 1 - alpha), at least 2) for mgrng_port_t.
"""
import math
import sys

W, ALPHA = 512, 2.0 ** -20


def critbinom(n, p, q):
    cdf = 0.0
    for k in range(n + 1):
        cdf += math.comb(n, k) * p ** k * (1 - p) ** (n - k)
        if cdf >= q:
            return k
    return n


def cutoffs(h):
    # The adaptive proportion test counts the window's first sample too: a cutoff under 2 would trip on every
    # window, so a high H (CRITBINOM 0) still gets 2.
    return 1 + math.ceil(20 / h), min(W, max(2, 1 + critbinom(W, 2.0 ** -h, 1 - ALPHA)))


if __name__ == "__main__":
    for a in sys.argv[1:] or ["0.5"]:
        h = float(a)
        h_milli = math.floor(h * 1000 + 1e-9)    # rounded down: never claim more entropy than was measured
        if h_milli < 1:
            sys.exit(f"H={a}: h_milli would be 0, which the code reads as the 0.5 placeholder")
        rct, apt = cutoffs(h_milli / 1000)       # the cutoffs for the H the port claims
        print(f"H={h}: h_milli={h_milli} rct_cutoff={rct} apt_cutoff={apt}")
