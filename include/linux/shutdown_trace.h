/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SHUTDOWN_TRACE_H
#define _LINUX_SHUTDOWN_TRACE_H

#include <linux/types.h>

struct device;

#ifdef CONFIG_SHUTDOWN_TRACE
void shutdown_trace_begin(void);
bool shutdown_trace_pre(struct device *dev);
void shutdown_trace_post(struct device *dev);
void shutdown_trace_end(void);
#else
static inline void shutdown_trace_begin(void) { }
static inline bool shutdown_trace_pre(struct device *dev) { return false; }
static inline void shutdown_trace_post(struct device *dev) { }
static inline void shutdown_trace_end(void) { }
#endif

#endif
