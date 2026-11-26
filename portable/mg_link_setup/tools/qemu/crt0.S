/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Bare-metal start for QEMU's RV32 virt machine (tools/qemu/run.sh). */

	.section .text.start
	.globl _start
_start:
	la	sp, _stack_top
	/* Paint the stack so main() can find the high-water mark. */
	la	t0, _stack_bottom
	li	t1, 0xa5a5a5a5
1:	sw	t1, 0(t0)
	addi	t0, t0, 4
	bltu	t0, sp, 1b
	/* Zero .bss. */
	la	t0, _bss_start
	la	t1, _bss_end
2:	bgeu	t0, t1, 3f
	sw	zero, 0(t0)
	addi	t0, t0, 4
	j	2b
3:	call	main
	/* sifive_test: power off. */
	li	t0, 0x100000
	li	t1, 0x5555
	sw	t1, 0(t0)
4:	j	4b
