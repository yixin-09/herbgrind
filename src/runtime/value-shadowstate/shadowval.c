/*--------------------------------------------------------------------*/
/*--- HerbGrind: a valgrind tool for Herbie            shadowval.c ---*/
/*--------------------------------------------------------------------*/

/*
   This file is part of HerbGrind, a valgrind tool for diagnosing
   floating point accuracy problems in binary programs and extracting
   problematic expressions.

   Copyright (C) 2016 Alex Sanchez-Stern

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

#include "shadowval.h"
#include "exprs.h"
#include "real.h"

#include "pub_tool_mallocfree.h"
#include "pub_tool_hashtable.h"
#include "pub_tool_libcbase.h"
#include "pub_tool_libcprint.h"
#include "pub_tool_libcassert.h"

int shadow_temps_made = 0;
VG_REGPARM(1) ShadowTemp* newShadowTemp(UWord num_vals){
  ShadowTemp* newShadowTemp =
    VG_(perm_malloc)(sizeof(ShadowTemp), vg_alignof(ShadowTemp));
  newShadowTemp->num_vals = num_vals;
  tl_assert(num_vals > 0);
  newShadowTemp->values =
    VG_(perm_malloc)(num_vals * sizeof(ShadowValue*), vg_alignof(ShadowValue*));
  return newShadowTemp;
}
void changeSingleValueType(ShadowTemp* temp, FloatType type){
  if (temp->values[0] != NULL){
    temp->values[0]->type = type;
  }
}
UWord hashDouble(double val){
  UWord result;
  VG_(memcpy)(&result, &val, sizeof(UWord));
  return result;
}
inline
ShadowValue* newShadowValue(FloatType type){
  ShadowValue* result =
    VG_(perm_malloc)(sizeof(ShadowValue), vg_alignof(ShadowValue));
  result->type = type;
  result->ref_count = 1;
  result->real = mkReal();
  return result;
}
ShadowValue* copyShadowValue(ShadowValue* val){
  ShadowValue* result = VG_(malloc)("shadow value", sizeof(ShadowValue));
  result->type = val->type;
  result->ref_count = 1;
  result->real = copyReal(val->real);
  result->expr = val->expr;
  result->influences = val->influences;
  return result;
}
VG_REGPARM(2) void assertValValid(const char* label, ShadowValue* val){
  tl_assert2(val->real != NULL, "%s: value is %p", label, val);
}
VG_REGPARM(2) void assertTempValid(const char* label, ShadowTemp* temp){
  for(int i = 0; i < temp->num_vals; ++i){
    assertValValid(label, temp->values[i]);
  }
}
VG_REGPARM(3) void assertNumVals(const char* label, ShadowTemp* temp,
                                 int num_vals){
  tl_assert2(temp->num_vals == num_vals,
             "%s: Expected %d vals in %p, got %d\n",
             label, num_vals, temp, temp->num_vals);
}
VG_REGPARM(3) void assertNumValsNot(const char* label,
                                    ShadowTemp* temp,
                                    int num_vals){
  tl_assert2(temp->num_vals != num_vals,
             "%s: Expected not %d vals in %p, got %d\n",
             label, num_vals, temp, temp->num_vals);
}