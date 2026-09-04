/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#ifndef __VULKAN_BRIDGE_64_H__
#define __VULKAN_BRIDGE_64_H__

class CPU64;

// The 64-bit lane's Vulkan hostcall (0x7fff0003). See
// source/vulkan/vulkanbridge64.cpp and docs/PLAN_WOW64_D3D9.md.
//
// op            operation index (BOXEDWINE_X64_VK_OP_*)
// argsAddress   guest pointer to an IN/OUT array of 64-bit argument words
// count         number of words, 0..BOXEDWINE_X64_VK_MAX_ARGS
//
// Returns the signed 64-bit result the guest shim reads back out of RAX: a
// VkResult for a command that has one, a BOXEDWINE_X64_VK_E_* code otherwise.
U64 vulkanBridge64(CPU64* cpu, U64 op, U64 argsAddress, U64 count);

// Capability bitmask reported by BOXEDWINE_X64_VK_OP_PROBE, also printed in
// the startup witness so a log says on every launch whether the lane could
// serve Vulkan at all.
U64 vulkanBridge64Capabilities(CPU64* cpu);

// The command name behind a KThread::diagnosticVulkanBridgeCall value: the
// dispatch index biased by one, so zero means the thread never called the
// bridge. Returns nullptr when the value names no command, which is how a
// caller outside this file reports "this thread did no Vulkan" without
// knowing anything about the command table.
const char* vulkanBridge64CommandName(U32 callPlusOne);

#endif
