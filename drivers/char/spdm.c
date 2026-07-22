// SPDX-License-Identifier: GPL-2.0-only
/*
 * Vendor-independent SPDM character-device core.
 *
 * Owns the creation and deletion of /dev/spdmN nodes and their sysfs
 * association. Vendor backends register via spdm_register(), supplying a
 * struct spdm_ops. The core owns the char-device file_operations and forwards
 * each operation to the backend under a per-device rwsem so that an unregister
 * cannot race an in-flight call. The core allocates no payload memory and
 * performs no SPDM communication.
 *
 * Modeled on the TPM chip core (drivers/char/tpm/tpm-chip.c).
 *
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 */

#include <linux/cdev.h>
#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/limits.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/slab.h>
#include <linux/spdm.h>

#define SPDM_DEV_MAX	256

/**
 * struct spdm_device - one registered SPDM char-device node
 * @dev:             embedded device (class member, parented to the registrant)
 * @cdev:            char device backing /dev/<name>
 * @ops_sem:         guards @ops against teardown while a call is in flight
 * @ops:             backend callbacks; set NULL by unregister to make backend
 *                   uncallable
 * @drvdata:         backend context
 * @minor:           IDR-allocated minor number
 * @sess_lock:       guards @active_count, @last_session_id and @sessions
 * @active_count:    number of currently-open sessions (files)
 * @last_session_id: monotonic candidate source for the next session id (u8,
 *                   wraps)
 * @sessions:        list of live struct spdm_file, walked for id-collision checks
 */
struct spdm_device {
	struct device		dev;
	struct cdev		cdev;
	struct rw_semaphore	ops_sem;
	const struct spdm_ops	*ops;
	void			*drvdata;
	int			minor;
	struct mutex		sess_lock;
	u32			active_count;
	u8			last_session_id;
	struct list_head	sessions;
};

/**
 * struct spdm_file - core-owned per-open state (file->private_data)
 * @session_id: non-zero id assigned at open, stable until release
 * @drvpriv:    backend per-open pointer (via spdm_set/get_filedata)
 * @owner:      backend module pinned at open; put at release. Cached here so the
 *              ref can be balanced even if the device is unregistered (ops set
 *              NULL) while this file is still open.
 * @node:       link into spdm_device.sessions
 */
struct spdm_file {
	u8			session_id;
	void			*drvpriv;
	struct module		*owner;
	struct list_head	node;
};

static dev_t spdm_devt;
static struct class *spdm_class;
static DEFINE_IDR(spdm_minor_idr);
static DEFINE_MUTEX(spdm_idr_lock);

static inline struct spdm_device *to_spdm_device(struct device *dev)
{
	return container_of(dev, struct spdm_device, dev);
}

void *spdm_drvdata(struct spdm_device *sdev)
{
	return sdev->drvdata;
}
EXPORT_SYMBOL_GPL(spdm_drvdata);

void spdm_set_filedata(struct file *file, void *data)
{
	struct spdm_file *sf = file->private_data;

	sf->drvpriv = data;
}
EXPORT_SYMBOL_GPL(spdm_set_filedata);

void *spdm_get_filedata(struct file *file)
{
	struct spdm_file *sf = file->private_data;

	return sf->drvpriv;
}
EXPORT_SYMBOL_GPL(spdm_get_filedata);

/* Caller must hold sdev->sess_lock. */
static bool spdm_id_in_use(struct spdm_device *sdev, u8 id)
{
	struct spdm_file *sf;

	list_for_each_entry(sf, &sdev->sessions, node)
		if (sf->session_id == id)
			return true;

	return false;
}

/*
 * Enforce the session cap, assign a collision-free non-zero id and publish
 * @sf on the device's session list. Returns -EBUSY once the cap is reached.
 */
static int spdm_session_add(struct spdm_device *sdev, struct spdm_file *sf)
{
	u8 sid;

	guard(mutex)(&sdev->sess_lock);

	if (sdev->active_count >= sdev->ops->max_session_count)
		return -EBUSY;

	do {
		sdev->last_session_id++;
		sid = sdev->last_session_id;
	} while (sid == 0 || spdm_id_in_use(sdev, sid));

	sf->session_id = sid;
	list_add(&sf->node, &sdev->sessions);
	sdev->active_count++;

	return 0;
}

static void spdm_session_del(struct spdm_device *sdev, struct spdm_file *sf)
{
	guard(mutex)(&sdev->sess_lock);

	list_del(&sf->node);
	sdev->active_count--;
}

/*
 * File operations. Each resolves the device from the cdev, takes the ops rwsem
 * for read, and forwards to the backend. A NULL ops (post-unregister) yields
 * -ENODEV.
 */

static int spdm_open(struct inode *inode, struct file *file)
{
	struct spdm_device *sdev = container_of(inode->i_cdev,
						struct spdm_device, cdev);
	struct spdm_file *sf __free(kfree) = NULL;
	int rc;

	stream_open(inode, file);

	guard(rwsem_read)(&sdev->ops_sem);

	if (!sdev->ops)
		return -ENODEV;

	sf = kzalloc_obj(*sf);
	if (!sf)
		return -ENOMEM;

	if (!try_module_get(sdev->ops->owner))
		return -ENODEV;
	sf->owner = sdev->ops->owner;

	rc = spdm_session_add(sdev, sf);
	if (rc) {
		module_put(sf->owner);
		return rc;
	}

	file->private_data = sf;

	if (sdev->ops->open) {
		rc = sdev->ops->open(sdev, file);
		if (rc) {
			file->private_data = NULL;
			spdm_session_del(sdev, sf);
			module_put(sf->owner);
			return rc;
		}
	}

	/*
	 * Committed: pin the device struct for the lifetime of this open file
	 * and hand ownership of @sf to file->private_data.
	 */
	get_device(&sdev->dev);
	retain_and_null_ptr(sf);

	return 0;
}

static int spdm_release(struct inode *inode, struct file *file)
{
	struct spdm_device *sdev = container_of(inode->i_cdev,
						struct spdm_device, cdev);
	struct spdm_file *sf __free(kfree) = file->private_data;

	scoped_guard(rwsem_read, &sdev->ops_sem)
		if (sdev->ops && sdev->ops->release)
			sdev->ops->release(sdev, file);

	spdm_session_del(sdev, sf);
	file->private_data = NULL;

	/* Balance the module ref taken at open, regardless of unregister state. */
	module_put(sf->owner);

	put_device(&sdev->dev);
	return 0;
}

static ssize_t spdm_read(struct file *file, char __user *buf,
			 size_t count, loff_t *ppos)
{
	struct spdm_device *sdev = container_of(file_inode(file)->i_cdev,
						struct spdm_device, cdev);
	struct spdm_file *sf = file->private_data;

	guard(rwsem_read)(&sdev->ops_sem);

	if (!sdev->ops)
		return -ENODEV;
	if (!sdev->ops->read)
		return -EINVAL;

	return sdev->ops->read(sdev, file, sf->session_id, buf, count);
}

static ssize_t spdm_write(struct file *file, const char __user *buf,
			  size_t count, loff_t *ppos)
{
	struct spdm_device *sdev = container_of(file_inode(file)->i_cdev,
						struct spdm_device, cdev);
	struct spdm_file *sf = file->private_data;

	guard(rwsem_read)(&sdev->ops_sem);

	if (!sdev->ops)
		return -ENODEV;
	if (!sdev->ops->write)
		return -EINVAL;

	return sdev->ops->write(sdev, file, sf->session_id, buf, count);
}

static long spdm_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct spdm_device *sdev = container_of(file_inode(file)->i_cdev,
						struct spdm_device, cdev);

	guard(rwsem_read)(&sdev->ops_sem);

	if (!sdev->ops)
		return -ENODEV;
	if (!sdev->ops->ioctl)
		return -ENOTTY;

	return sdev->ops->ioctl(sdev, file, cmd, arg);
}

static const struct file_operations spdm_fops = {
	.owner		= THIS_MODULE,
	.open		= spdm_open,
	.release	= spdm_release,
	.read		= spdm_read,
	.write		= spdm_write,
	.unlocked_ioctl	= spdm_ioctl,
};

static void spdm_dev_release(struct device *dev)
{
	struct spdm_device *sdev = to_spdm_device(dev);

	scoped_guard(mutex, &spdm_idr_lock)
		idr_remove(&spdm_minor_idr, sdev->minor);

	kfree(sdev);
}

struct spdm_device *spdm_register(struct device *parent,
				  const struct spdm_ops *ops,
				  void *drvdata)
{
	struct spdm_device *sdev;
	int rc;

	/*
	 * A parent is required: the node name is core-assigned (spdmN) and the
	 * caller identifies the target device via the /sys/class/spdm/spdmN/device
	 * symlink, which only exists when the class device has a parent.
	 *
	 * max_session_count must fit the u8 session-id space (1-255): 0 permits
	 * no sessions, and >255 cannot be honored.
	 */
	if (!parent || !ops || !ops->owner)
		return ERR_PTR(-EINVAL);
	if (ops->max_session_count == 0 || ops->max_session_count > U8_MAX)
		return ERR_PTR(-EINVAL);

	sdev = kzalloc_obj(*sdev);
	if (!sdev)
		return ERR_PTR(-ENOMEM);

	init_rwsem(&sdev->ops_sem);
	mutex_init(&sdev->sess_lock);
	INIT_LIST_HEAD(&sdev->sessions);
	sdev->ops = ops;
	sdev->drvdata = drvdata;

	scoped_guard(mutex, &spdm_idr_lock)
		rc = idr_alloc(&spdm_minor_idr, NULL, 0, SPDM_DEV_MAX, GFP_KERNEL);
	if (rc < 0) {
		kfree(sdev);
		return ERR_PTR(rc);
	}
	sdev->minor = rc;

	device_initialize(&sdev->dev);
	sdev->dev.class   = spdm_class;
	sdev->dev.parent  = parent;
	sdev->dev.devt    = MKDEV(MAJOR(spdm_devt), sdev->minor);
	sdev->dev.release = spdm_dev_release;

	/* Core-owned name; callers cannot influence it. */
	rc = dev_set_name(&sdev->dev, "spdm%d", sdev->minor);
	if (rc)
		goto err_put;

	cdev_init(&sdev->cdev, &spdm_fops);
	sdev->cdev.owner = THIS_MODULE;

	rc = cdev_device_add(&sdev->cdev, &sdev->dev);
	if (rc) {
		dev_err(parent, "spdm: cdev_device_add failed (%d)\n", rc);
		goto err_put;
	}

	/* Publish now that the device is fully live. */
	scoped_guard(mutex, &spdm_idr_lock)
		idr_replace(&spdm_minor_idr, sdev, sdev->minor);

	return sdev;

err_put:
	/* spdm_dev_release frees the struct and releases the minor. */
	put_device(&sdev->dev);
	return ERR_PTR(rc);
}
EXPORT_SYMBOL_GPL(spdm_register);

void spdm_unregister(struct spdm_device *sdev)
{
	if (IS_ERR_OR_NULL(sdev))
		return;

	cdev_device_del(&sdev->cdev, &sdev->dev);

	scoped_guard(mutex, &spdm_idr_lock)
		idr_replace(&spdm_minor_idr, NULL, sdev->minor);

	/* Make the backend uncallable for any fd still open. */
	scoped_guard(rwsem_write, &sdev->ops_sem)
		sdev->ops = NULL;

	put_device(&sdev->dev);
}
EXPORT_SYMBOL_GPL(spdm_unregister);

static int __init spdm_init(void)
{
	int rc;

	rc = alloc_chrdev_region(&spdm_devt, 0, SPDM_DEV_MAX, "spdm");
	if (rc)
		return rc;

	spdm_class = class_create("spdm");
	if (IS_ERR(spdm_class)) {
		rc = PTR_ERR(spdm_class);
		goto err_chrdev;
	}

	return 0;

err_chrdev:
	unregister_chrdev_region(spdm_devt, SPDM_DEV_MAX);
	return rc;
}

static void __exit spdm_exit(void)
{
	class_destroy(spdm_class);
	unregister_chrdev_region(spdm_devt, SPDM_DEV_MAX);
	idr_destroy(&spdm_minor_idr);
}

module_init(spdm_init);
module_exit(spdm_exit);

MODULE_DESCRIPTION("Vendor-independent SPDM char-device core");
MODULE_AUTHOR("AMD");
MODULE_LICENSE("GPL");
