//
//  py_types.h
//  libc-ray CPython wrapper
//
//  Created by Valtteri on 9.2.2025
//  Copyright © 2025 Valtteri Koskivuori. All rights reserved.
//

#ifndef PY_TYPES_H
#define PY_TYPES_H

/*
	Note: We define _POSIX_C_SOURCE on the command line because our includes
	are a huge mess, for the time being. Python wants 200809L instead of our
	default of '200112L', so we make an exception here to suppress warnings.
*/
#ifdef _POSIX_C_SOURCE
	#undef _POSIX_C_SOURCE
#endif
#include <Python.h>
#include <c-ray/c-ray.h>

extern PyTypeObject type_py_vector;
typedef struct {
	PyObject_HEAD
	struct cr_vector val;
} py_vector;

extern PyTypeObject type_py_coord;
typedef struct {
	PyObject_HEAD
	struct cr_coord val;
} py_coord;

extern PyTypeObject type_py_bitmap;
typedef struct {
	PyObject_HEAD
	struct cr_bitmap *ref;
} py_bitmap;
PyObject *py_bitmap_wrap(struct cr_bitmap *ref);

extern PyTypeObject type_py_renderer_cb_info;
typedef struct {
	PyObject_HEAD
	struct cr_renderer_cb_info info;
} py_renderer_cb_info;


struct cr_python_type {
	PyTypeObject *py_object;
	char *py_name;
};
extern struct cr_python_type all_types[];
#endif
