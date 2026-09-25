/* SPDX-License-Identifier: GPL-2.0 */
/* Cosmo boot instrument (arch/arm64/kernel/cosmo-mark.c): secondary-CPU arrival marks. */
#ifndef __ASM_COSMO_MARK_H
#define __ASM_COSMO_MARK_H
#include <linux/types.h>
void cosmo_secondary_arrived(u64 mpidr);
void cosmo_secondary_clear(void);
#endif
