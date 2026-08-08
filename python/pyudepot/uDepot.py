'''
  Copyright (c) 2020 International Business Machines
  All rights reserved.

  SPDX-License-Identifier: BSD-3-Clause

  Authors: Nikolas Ioannou (nio@zurich.ibm.com),
'''

import ctypes
from ctypes import *
import platform
import os
import numpy as np
import logging
import atexit
from numpy.ctypeslib import ndpointer

if platform.system() == 'Windows':
    raise OSError(22, 'Unsupported OS', 'windows')
else:
    # Prefer the library shipped alongside this module, so importing pyudepot
    # works from a build tree without LD_LIBRARY_PATH being set before the
    # interpreter starts (which a test harness cannot do for itself). Fall
    # back to the loader search path for installed layouts.
    _lib_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             'libpyudepot.so')
    libudepot = cdll.LoadLibrary(
        _lib_path if os.path.exists(_lib_path) else 'libpyudepot.so')

pyopen = libudepot.uDepotOpen
pyopen_backend = libudepot.uDepotOpenWithBackend
pyclose = libudepot.uDepotClose
pyget = libudepot.uDepotGet
pyput = libudepot.uDepotPut
pydel = libudepot.uDepotDel
pyexists = libudepot.uDepotExists


pyopen.argtypes  = [c_char_p, c_ulonglong, c_int]
pyopen_backend.argtypes = [c_char_p, c_ulonglong, c_int, c_int]
# Both open calls return a KV* . Without an explicit restype ctypes assumes
# c_int and truncates the 64-bit handle to 32 bits -- 0x555555cd8a00 comes
# back as 0x55cd8a00 -- and the next call segfaults dereferencing it. Whether
# it crashes depends on where the allocator happens to place the object, so
# this hid for a long time and then failed reliably on one machine and not
# another.
pyopen.restype = c_void_p
pyopen_backend.restype = c_void_p
pyclose.argtypes = [c_void_p]
pyget.argtypes   = [c_void_p, ndpointer(ctypes.c_ubyte, flags="C_CONTIGUOUS"), c_uint, ndpointer(ctypes.c_ubyte, flags="C_CONTIGUOUS"), c_ulonglong]
pyput.argtypes   = [c_void_p, ndpointer(ctypes.c_ubyte, flags="C_CONTIGUOUS"), c_uint, ndpointer(ctypes.c_ubyte, flags="C_CONTIGUOUS"), c_ulonglong]
pydel.argtypes   = [c_void_p, ndpointer(ctypes.c_ubyte, flags="C_CONTIGUOUS"), c_uint]
pyexists.argtypes = [c_void_p, ndpointer(ctypes.c_ubyte, flags="C_CONTIGUOUS"), c_uint, POINTER(c_ulonglong)]
pyexists.restype  = c_int
BACKEND_POSIX = 2
BACKEND_O_DIRECT = 3
BACKEND_SPDK = 4
BACKEND_TRT_AIO = 5
BACKEND_TRT_URING = 6
BACKEND_TRT_SPDK = 7

class uDepot:
    def __init__(self, **kwargs):
        self._fname = kwargs.get('file_name', '/tmp/pyudepot-test')
        self._size = kwargs.get('size', 1024*1024+4096)
        self._backend = kwargs.get('backend', 0)
        self._force_destroy = 1 if kwargs.get('force_destroy', False) else 0
        if self._backend:
            self._kv = pyopen_backend(
                self._fname.encode('utf-8'), self._size,
                self._force_destroy, self._backend)
        else:
            self._kv = pyopen(
                self._fname.encode('utf-8'), self._size, self._force_destroy)
        if 0 == self._kv:
            raise IOError('failed to spawn uDepot for {} (backend={})'.format(
                self._fname, self._backend))
        atexit.register(self.__cleanup)

    def __cleanup(self):
        if self._kv:
            pyclose(self._kv)

    # key: np with key
    # val_out: np array to get the data to (e.g., np.empty(VALSIZE_BYTES))
    # returns True if get successful and data in val_out
    # False if data not found
    def get(self, key, val_out):
        rc = pyget(self._kv, key, key.size, val_out, val_out.size)
        if 0 != rc:
            logging.info('pyget returned={}'.format(rc))
            return False
        return True

    # key: np with key
    # val: empty np array (e.g., np.empty(VALSIZE_BYTES))
    def put(self, key, val):
        rc=pyput(self._kv, key, key.size, val, val.size)
        if 0 != rc:
            logging.info('pyput returned={}'.format(rc))
            return False
        return True

    # key: np with key
    # returns True if deleted, False if not found
    def delete(self, key):
        rc = pydel(self._kv, key, key.size)
        if 0 != rc:
            logging.info('pydel returned={}'.format(rc))
            return False
        return True

    # key: np with key
    # returns value size in bytes, or None if not found
    def exists(self, key):
        val_size = c_ulonglong(0)
        rc = pyexists(self._kv, key, key.size, byref(val_size))
        if 0 != rc:
            return None
        return val_size.value

