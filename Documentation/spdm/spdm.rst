.. SPDX-License-Identifier: GPL-2.0

========================
SPDM Userspace Interface
========================

SPDM (Security Protocol and Data Model) is a standard defined by DMTF
specifying messages and a handshake protocol for device authentication,
firmware measurement, and secure sessions.

This module is intended to provide a single point of contact for all devices
that wish to export an SPDM interface to userspace.

What it does not do
===================

This module does not dictate how a driver relays SPDM commands to its target.
It may use PCIe DOE, MCTP over I3C, or a custom interface.

What it does do
===============

This module does several things:

- It exports a structure that drivers can fill out with callbacks that
  implement send/receive mechanics for communicating with SPDM devices.
- It manages the creation/deletion of character device nodes, identified by
  ``/dev/spdm%d``.
- It manages accesses to the nodes.

  - Each open() is a distinct session. A backend declares how many concurrent
    sessions it supports via ``max_session_count`` (1-255); the core enforces
    that ceiling and returns ``-EBUSY`` from open() once it is reached.
  - The core assigns each open a non-zero session id (see below) that is stable
    until release(). Ids are unique among a node's currently-live sessions; a
    freed id may be reused by a later open.

- It specifies the format of requests and responses.

Why?
====

AMD is initiating the development of this as part of an effort to make the SPDM
responder available in some devices accessible to user space. Particularly this
impacts client devices, where this functionality is new and there is typically
no secondary interface. In the future it may also apply to server platforms.

In doing this, AMD is taking to account the industry standard nature of SPDM and
wants to ensure that, should other device vendors decide to export their SPDM
responders to userspace, a common method exists for creating device nodes
without conflict.

Host Library Support
====================

The baseline expectation is this interface can be utilized by spdm-emu [1],
with updates, and without changes to libspdm [2].

Design
======

For the SPDM layer, the interface is as minimal as possible: a direct
passthrough of select file operations and semaphores to protect accesses.

Data Structures
---------------

The spdm_ops struct will contain function pointers for

* open
* release
* read
* write
* ioctl

These are loosely based on their file_operations counterparts, but are not
registered directly and do not mirror them exactly. The read and write callbacks
additionally take the per-open session id (a u8) so the backend can route the
request to the session's buffers; they omit the file-position argument, which is
meaningless for this message interface. Drivers will create and submit an
spdm_ops structure during init.

The spdm_ops struct also carries ``max_session_count``, the maximum number of
concurrent sessions the backend supports on a node (1-255).

Because the core owns ``file->private_data``, backends store and retrieve their
own per-open context via ``spdm_set_filedata()`` and ``spdm_get_filedata()``
rather than touching ``file->private_data`` directly.

Message Data Format
-------------------

Messages will consist of:

- A 1-byte message type field defined by MCTP. This is done automatically by
  libspdm using libspdm_mctp_encode_message and is processed by
  libspdm_mctp_decode_message.

This is compatible with the spdm-emu command-line argument ``--trans=MCTP``.

APIs
----

Drivers will call the following functions to create and destroy their nodes:

* spdm_register - Accepts a device, operations struct, and private data.
* spdm_unregister - Removes a device.
* spdm_drvdata - Returns the private data reference.
* spdm_set_filedata - Stores the backend's per-open session pointer.
* spdm_get_filedata - Returns the backend's per-open session pointer.

The device paramter of spdm_register is mandatory, as the reference is used
to establish a link between the device node and /sys/class/spdm/spdmN/device,
ensuring there is clear connection between the spdm character device and the
physical device it connects to.

References
==========

[1] https://github.com/dmtf/spdm-emu

[2] https://github.com/DMTF/libspdm
