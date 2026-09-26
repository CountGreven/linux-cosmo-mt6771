/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SHUTDOWN_TRACE_H
#define _LINUX_SHUTDOWN_TRACE_H

struct device;

#ifdef CONFIG_SHUTDOWN_TRACE
void shutdown_trace_begin(void);
void shutdown_trace_pre(struct device *dev);
void shutdown_trace_post(struct device *dev);
void shutdown_trace_end(void);
#else
static inline void shutdown_trace_begin(void) { }
static inline void shutdown_trace_pre(struct device *dev) { }
static inline void shutdown_trace_post(struct device *dev) { }
static inline void shutdown_trace_end(void) { }
#endif

#endif
