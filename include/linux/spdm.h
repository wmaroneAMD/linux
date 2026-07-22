/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Vendor-independent SPDM character-device core.
 *
 * The core owns the lifecycle of /dev/spdmN nodes and their sysfs
 * association. Vendor backends register with it, supplying callbacks for the
 * actual SPDM operations. The core performs no payload allocation and no SPDM
 * communication of its own.
 *
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 */

#ifndef _LINUX_SPDM_H
#define _LINUX_SPDM_H

#include <linux/types.h>

struct spdm_device;
struct device;
struct file;
struct module;

/**
 * struct spdm_ops - backend callbacks invoked by the SPDM core
 * @owner:             backend module, pinned for the duration of each open file
 * @max_session_count: maximum number of concurrent sessions (open files) the
 *                     backend supports on a node; must be in the range 1-255.
 *                     The core rejects registration outside that range and
 *                     returns -EBUSY from open() once the cap is reached.
 * @open:    called on open(); backend allocates its per-open session and
 *           stores it via spdm_set_filedata()
 * @release: called on the final close() of a file opened via @open
 * @read:    called on read(); @session_id identifies the calling open's session
 *           so the backend can route to the right buffers; returns bytes
 *           produced or -errno
 * @write:   called on write(); @session_id identifies the calling open's
 *           session; returns bytes consumed or -errno
 * @ioctl:   called on unlocked_ioctl(); returns 0/positive or -errno
 *
 * All callbacks are invoked with the core's per-device lock held for read, so a
 * concurrent spdm_unregister() cannot tear the backend down underneath an
 * in-flight call. Backends must not call spdm_unregister() from within a
 * callback.
 *
 * @session_id is a non-zero per-open identifier assigned by the core at open()
 * and stable until release(). It is unique among a node's currently-live
 * sessions; a freed id may be reused by a later open.
 */
struct spdm_ops {
	struct module *owner;
	u32	max_session_count;
	int	(*open)(struct spdm_device *sdev, struct file *file);
	int	(*release)(struct spdm_device *sdev, struct file *file);
	ssize_t	(*read)(struct spdm_device *sdev, struct file *file,
			u8 session_id, char __user *buf, size_t count);
	ssize_t	(*write)(struct spdm_device *sdev, struct file *file,
			 u8 session_id, const char __user *buf, size_t count);
	long	(*ioctl)(struct spdm_device *sdev, struct file *file,
			 unsigned int cmd, unsigned long arg);
};

/**
 * spdm_register() - create a /dev/spdmN node backed by @ops
 * @parent:  registering device (e.g. &pdev->dev). The core does not let the
 *           caller influence the node name; instead it names the node spdmN
 *           from a core-assigned minor and associates it with @parent. Callers
 *           and users identify the target device by following the sysfs
 *           /sys/class/spdm/spdmN/device symlink back to @parent. @parent is
 *           therefore required.
 * @ops:     backend callback vtable; must remain valid until unregister
 * @drvdata: backend context, retrievable in callbacks via spdm_drvdata()
 *
 * Return: device handle on success, ERR_PTR() on failure.
 */
struct spdm_device *spdm_register(struct device *parent,
				  const struct spdm_ops *ops,
				  void *drvdata);

/**
 * spdm_unregister() - remove a device created by spdm_register()
 * @sdev: handle returned by spdm_register()
 *
 * Removes the /dev node and makes the backend uncallable. Open file
 * descriptors remain valid but their subsequent operations fail with -ENODEV.
 */
void spdm_unregister(struct spdm_device *sdev);

/**
 * spdm_drvdata() - retrieve the backend context for a device
 * @sdev: device handle passed to a callback
 */
void *spdm_drvdata(struct spdm_device *sdev);

/**
 * spdm_set_filedata() - store the backend's per-open session pointer
 * @file: the open file (typically from the @open callback)
 * @data: backend per-open context
 *
 * The core owns file->private_data; backends must use this accessor rather than
 * assigning it directly.
 */
void spdm_set_filedata(struct file *file, void *data);

/**
 * spdm_get_filedata() - retrieve the backend's per-open session pointer
 * @file: the open file passed to a callback
 *
 * Return: the pointer previously set with spdm_set_filedata(), or NULL.
 */
void *spdm_get_filedata(struct file *file);

#endif /* _LINUX_SPDM_H */
