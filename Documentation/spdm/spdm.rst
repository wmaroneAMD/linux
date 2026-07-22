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
- It manages accesses to the nodes. Each open() is a distinct session with a
  core-assigned session id; a backend declares how many concurrent sessions it
  supports and the core enforces that ceiling. See ``struct spdm_ops`` below.
- It specifies the format of requests and responses.

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

Drivers create and submit a ``struct spdm_ops`` during init. Its callbacks are
loosely based on their file_operations counterparts, but are not registered
directly and do not mirror them exactly: read and write take the per-open
session id in place of the file-position argument, which is meaningless for
this message interface.

.. kernel-doc:: include/linux/spdm.h
   :identifiers: spdm_ops

Message Data Format
-------------------

Messages will consist of:

- A 1-byte message type field defined by MCTP. This is done automatically by
  libspdm using libspdm_mctp_encode_message and is processed by
  libspdm_mctp_decode_message.

This is compatible with the spdm-emu command-line argument ``--trans=MCTP``.

APIs
----

Drivers call spdm_register() and spdm_unregister() to create and destroy their
nodes. Because the core owns ``file->private_data``, backends store and retrieve
their own per-open context via spdm_set_filedata() and spdm_get_filedata()
rather than touching ``file->private_data`` directly.

.. kernel-doc:: include/linux/spdm.h
   :identifiers: spdm_register spdm_unregister spdm_drvdata
                 spdm_set_filedata spdm_get_filedata

References
==========

[1] https://github.com/dmtf/spdm-emu

[2] https://github.com/DMTF/libspdm
