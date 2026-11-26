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

/* QEMU RV32 virt: RAM at 0x80000000. */
ENTRY(_start)
MEMORY { RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 1M }
SECTIONS
{
	.text : { *(.text.start) *(.text*) } > RAM
	.rodata : { *(.rodata*) *(.srodata*) } > RAM
	.data : { *(.data*) *(.sdata*) } > RAM
	.bss (NOLOAD) : ALIGN(4) { _bss_start = .; *(.bss*) *(.sbss*) *(COMMON) . = ALIGN(4); _bss_end = .; } > RAM
	.stack (NOLOAD) : ALIGN(16) { _stack_bottom = .; . += 16K; _stack_top = .; } > RAM
}
