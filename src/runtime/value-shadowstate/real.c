/*--------------------------------------------------------------------*/
/*--- HerbGrind: a valgrind tool for Herbie                 real.c ---*/
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

#include "real.h"

#include "../../options.h"
#include "pub_tool_mallocfree.h"
#include "pub_tool_libcprint.h"

Real mkReal(double bytes){
  Real result = VG_(malloc)("real", sizeof(struct _RealStruct));
  mpfr_init2(result->mpfr_val, precision);
  mpfr_set_d(result->mpfr_val, bytes, MPFR_RNDN);
  return result;
}
void setReal(Real r, double bytes){
  mpfr_set_d(r->mpfr_val, bytes, MPFR_RNDN);
}
void freeReal(Real real){
  mpfr_clear(real->mpfr_val);
  VG_(free)(real);
}

double getDouble(Real real){
  return mpfr_get_d(real->mpfr_val, MPFR_RNDN);
}

Real copyReal(Real real){
  Real result = VG_(malloc)("real", sizeof(struct _RealStruct));
  mpfr_init2(result->mpfr_val, precision);
  mpfr_set(result->mpfr_val, real->mpfr_val, MPFR_RNDN);
  return result;
}
