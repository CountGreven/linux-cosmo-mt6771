/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Stand-in for the vendor mt-plat headers the CONSYS stack includes.
 *
 * Every service that came from mt-plat/ in the 4.4 tree is either mapped to
 * a mainline API here or is a stub that logs and returns -ENOSYS. The map is
 * in projects/cosmo/17-consys-port.org. A stub is a STAND-IN for a real
 * implementation, marked as such below; none of them power the radio.
 */
#ifndef MTK_PLAT_SHIM_H
#define MTK_PLAT_SHIM_H

#endif /* MTK_PLAT_SHIM_H */
