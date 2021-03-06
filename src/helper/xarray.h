/*--------------------------------------------------------------------*/
/*--- Herbgrind: a valgrind tool for Herbie               xarray.h ---*/
/*--------------------------------------------------------------------*/

/*
   This file is part of Herbgrind, a valgrind tool for diagnosing
   floating point accuracy problems in binary programs and extracting
   problematic expressions.

   Copyright (C) 2016-2017 Alex Sanchez-Stern

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 3 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307, USA.

   The GNU General Public License is contained in the file COPYING.
*/

#ifndef _XARRAY_H
#define _XARRAY_H

#include "pub_tool_libcassert.h"
#include "pub_tool_mallocfree.h"

#ifndef INITIAL_XARRAY_CAPACITY
#define INITIAL_XARRAY_CAPACITY 4
#endif

#define Xarray_H(Type, name)                    \
  typedef struct _##name                        \
  {                                             \
    int size;                                   \
    int capacity;                               \
    Type* data;                                 \
  }* name;                                      \
  name mkXA_##name(void);                       \
  void XApush_##name(name arr, Type item);      \
  void freeXA_##name(name XA);                  \
  name mkXA_w_initial_##name(int initial);             \
  void finalizeXA_##name(name arr);

#define Xarray_Impl(Type, name)                         \
  name mkXA_##name() {                                  \
    return mkXA_w_initial_##name(INITIAL_XARRAY_CAPACITY);     \
  }                                                                     \
  void XApush_##name(name arr, Type item){                              \
    if (arr->capacity == arr->size){                                    \
      arr->data =                                                       \
        VG_(realloc)("xarray data", arr->data,                          \
                     arr->capacity * 2 * sizeof(Type));                 \
      arr->capacity *= 2;                                               \
    }                                                                   \
    arr->data[arr->size++] = item;                                      \
  }                                                                     \
  void freeXA_##name(name XA) {                                         \
    VG_(free)(XA->data);                                                \
    VG_(free)(XA);                                                      \
  }                                                                     \
  void finalizeXA_##name(name arr){                                     \
    VG_(realloc_shrink)(arr->data, arr->size);                          \
  }                                                                     \
  name mkXA_w_initial_##name(int initial){                              \
    name arr = VG_(malloc)("xarray",                   \
                           sizeof(struct _##name));     \
    arr->size = 0;                                     \
    arr->capacity = INITIAL_XARRAY_CAPACITY;           \
    arr->data =                                                        \
      VG_(malloc)("xarray data",                                        \
                  sizeof(Type) * INITIAL_XARRAY_CAPACITY);              \
    return arr;                                                         \
  }
    

#define mkXA(type) mkXA_##type
#define mkXA_w_initial(type) mkXA_w_initial_##type
#define XApush(type) XApush_##type
#define freeXA(type) freeXA_##type
#define finalizeXA(type) finalizeXA_##type

#endif
