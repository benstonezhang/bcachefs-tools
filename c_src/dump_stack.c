/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * C implementation of bch2_demangle.
 * Ported from src/dump_stack.rs.
 */

#include <string.h>

#include "libbcachefs.h"

/**
 * bch2_demangle - Demangle a symbol name
 * @mangled: Mangled name
 * @out: Buffer to write demangled name to
 * @out_len: Size of out buffer
 *
 * Writes a NUL-terminated demangled form into out[0..out_len]. If the input
 * is not mangled, returns it unchanged. Returns the number of bytes written
 * (excluding NUL), or 0 if out_len == 0.
 *
 * In the pure C version, we don't have a Rust demangler library.
 * We just copy the string unchanged.
 */
size_t bch2_demangle(const char *mangled, char *out, size_t out_len)
{
	if (!out_len || !mangled || !out)
		return 0;

	size_t len = strlen(mangled);
	size_t n = len < out_len - 1 ? len : out_len - 1;

	memcpy(out, mangled, n);
	out[n] = '\0';

	return n;
}
