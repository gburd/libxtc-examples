/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/t_stack.h
 *	Test-only: T_SQL_STACK, the stack size for a test fiber that runs
 *	SQL.  AddressSanitizer pads every frame with redzones:
 *	vx_run_write_ast() alone takes 58 KB under ASan (28 KB natively),
 *	past the 64 KiB default fiber stack.  ASan builds give the SQL
 *	fiber the server's 512 KiB stack (main.c); other builds keep the
 *	default (0).  gcc defines __SANITIZE_ADDRESS__; clang answers
 *	__has_feature(address_sanitizer).
 */
#ifndef SQLXTC_T_STACK_H
#define SQLXTC_T_STACK_H

#if defined(__SANITIZE_ADDRESS__)
#define T_SQL_STACK (512u * 1024u)
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define T_SQL_STACK (512u * 1024u)
#endif
#endif
#ifndef T_SQL_STACK
#define T_SQL_STACK 0u   /* the runtime default */
#endif

#endif /* SQLXTC_T_STACK_H */
