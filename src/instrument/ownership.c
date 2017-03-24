/*--------------------------------------------------------------------*/
/*--- HerbGrind: a valgrind tool for Herbie            ownership.c ---*/
/*--------------------------------------------------------------------*/

/*
   This file is part of HerbGrind, a valgrind tool for diagnosing
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

#include "ownership.h"
#include "pub_tool_xarray.h"
#include "pub_tool_mallocfree.h"
#include "pub_tool_machine.h"
#include "pub_tool_libcprint.h"
#include "../runtime/value-shadowstate/value-shadowstate.h"
#include "../helper/instrument-util.h"

XArray* tempDebt;

void initOwnership(void){
  tempDebt = VG_(newXA)(VG_(malloc), "temp debt array",
                        VG_(free), sizeof(IRTemp));
}
void cleanupBlockOwnership(IRSB* sbOut, IRExpr* guard){
  if (VG_(sizeXA)(tempDebt) == 0){
    return;
  }
  IRTemp* curDebtContents =
    VG_(perm_malloc)(sizeof(IRTemp) * VG_(sizeXA)(tempDebt),
                     vg_alignof(IRTemp));
  for(int i = 0; i < VG_(sizeXA)(tempDebt); ++i){
    curDebtContents[i] = *(IRTemp*)VG_(indexXA)(tempDebt, i);
  }
  IRDirty* dynCleanupDirty =
    unsafeIRDirty_0_N(2, "dynamicCleanup",
                      VG_(fnptr_to_fnentry)(dynamicCleanup),
                      mkIRExprVec_2(mkU64(VG_(sizeXA)(tempDebt)),
                                    mkU64((uintptr_t)curDebtContents)));
  dynCleanupDirty->mFx = Ifx_Modify;
  dynCleanupDirty->guard = guard;
  dynCleanupDirty->mAddr = mkU64((uintptr_t)shadowTemps);
  dynCleanupDirty->mSize = sizeof(ShadowTemp) * MAX_TEMPS;
  addStmtToIRSB(sbOut, IRStmt_Dirty(dynCleanupDirty));
}

void resetOwnership(IRSB* sbOut){
  VG_(deleteXA)(tempDebt);
  tempDebt = VG_(newXA)(VG_(malloc), "temp debt array", VG_(free),
                        sizeof(IRTemp));
}

void cleanupAtEndOfBlock(IRSB* sbOut, IRTemp shadowed_temp){
  for(int i = 0; i < VG_(sizeXA)(tempDebt); ++i){
    IRTemp* prevEntry = VG_(indexXA)(tempDebt, i);
    if (*prevEntry == shadowed_temp){
      return;
    }
  }
  VG_(addToXA)(tempDebt, &shadowed_temp);
}
void addDynamicDisown(IRSB* sbOut, IRTemp idx){
  IRDirty* disownDirty =
    unsafeIRDirty_0_N(1, "disownShadowTempDynamic",
                      VG_(fnptr_to_fnentry)(disownShadowTempDynamic),
                      mkIRExprVec_1(mkU64(idx)));
  disownDirty->mFx = Ifx_Modify;
  disownDirty->mAddr = mkU64((uintptr_t)&(shadowTemps[idx]));
  disownDirty->mSize = sizeof(ShadowTemp*);
  addStmtToIRSB(sbOut, IRStmt_Dirty(disownDirty));
}
void addDynamicDisownNonNull(IRSB* sbOut, IRTemp idx){
  IRDirty* disownDirty =
    unsafeIRDirty_0_N(1, "disownShadowTempNonNullDynamic",
                      VG_(fnptr_to_fnentry)(disownShadowTempNonNullDynamic),
                      mkIRExprVec_1(mkU64(idx)));
  disownDirty->mFx = Ifx_Modify;
  disownDirty->mAddr = mkU64((uintptr_t)&(shadowTemps[idx]));
  disownDirty->mSize = sizeof(ShadowTemp*);
  addStmtToIRSB(sbOut, IRStmt_Dirty(disownDirty));
}
void addDynamicDisownNonNullDetached(IRSB* sbOut, IRExpr* st){
  runPureCCall(sbOut,
               mkIRCallee(1, "disownShadowTemp",
                          VG_(fnptr_to_fnentry)(disownShadowTemp)),
               Ity_I64,
               mkIRExprVec_1(st));
}
void addDisownNonNull(IRSB* sbOut, IRExpr* shadow_temp, int num_vals){
  IRExpr* valuesAddr = runArrow(sbOut, shadow_temp, ShadowTemp, values);
  for(int i = 0; i < num_vals; ++i){
    IRExpr* value = runIndex(sbOut, valuesAddr, ShadowValue*, i);
    addSVDisown(sbOut, value);
  }
  addStackPush(sbOut, freedTemps[num_vals - 1], shadow_temp);
}
void addDisown(IRSB* sbOut, IRExpr* shadow_temp, int num_vals){
  IRExpr* tempNonNull = runNonZeroCheck64(sbOut, shadow_temp);
  IRExpr* valuesAddr =
    runArrowG(sbOut, tempNonNull, shadow_temp, ShadowTemp, values);
  for(int i = 0; i < num_vals; ++i){
    IRExpr* value = runIndexG(sbOut, tempNonNull,
                              valuesAddr,
                              ShadowValue*, i);
    addSVDisownG(sbOut, tempNonNull, value);
  }
  addStackPushG(sbOut, tempNonNull, freedTemps[num_vals - 1],
                shadow_temp);
}
void addDisownG(IRSB* sbOut, IRExpr* guard, IRExpr* shadow_temp, int num_vals){
  IRExpr* valuesAddr =
    runArrowG(sbOut, guard, shadow_temp, ShadowTemp, values);
  for(int i = 0; i < num_vals; ++i){
    IRExpr* value = runIndexG(sbOut, guard,
                              valuesAddr,
                              ShadowValue*, i);
    addSVDisownG(sbOut, guard, value);
  }
  addStackPushG(sbOut, guard, freedTemps[num_vals - 1],
                shadow_temp);
}
void addSVOwn(IRSB* sbOut, IRExpr* sv){
  IRExpr* valueNonNull = runNonZeroCheck64(sbOut, sv);
  addSVOwnNonNullG(sbOut, valueNonNull, sv);
}
void addSVOwnNonNullG(IRSB* sbOut, IRExpr* guard, IRExpr* sv){
  IRExpr* prevRefCount =
    runArrowG(sbOut, guard, sv, ShadowValue, ref_count);
  IRExpr* newRefCount =
    runBinop(sbOut, Iop_Add64, prevRefCount, mkU64(1));
  addStoreArrowG(sbOut, guard, sv, ShadowValue, ref_count, newRefCount);
  if (PRINT_VALUE_MOVES){
    addPrintG3(guard, "[2] Owning %p, new ref_count %d\n", sv, newRefCount);
  }
}
void addSVOwnNonNull(IRSB* sbOut, IRExpr* sv){
  IRExpr* prevRefCount =
    runArrow(sbOut, sv, ShadowValue, ref_count);
  IRExpr* newRefCount =
    runBinop(sbOut, Iop_Add64, prevRefCount, mkU64(1));
  if (PRINT_VALUE_MOVES){
    addPrint3("[3] Owning %p, new ref_count %d\n", sv, newRefCount);
  }
  addStoreArrow(sbOut, sv, ShadowValue, ref_count, newRefCount);
}
void addSVDisown(IRSB* sbOut, IRExpr* sv){
  IRExpr* valueNonNull = runNonZeroCheck64(sbOut, sv);
  addSVDisownNonNullG(sbOut, valueNonNull, sv);
}
void addSVDisownNonNull(IRSB* sbOut, IRExpr* sv){
  IRExpr* prevRefCount =
    runArrow(sbOut, sv, ShadowValue, ref_count);
  IRExpr* lastRef = runBinop(sbOut, Iop_CmpEQ64, prevRefCount, mkU64(1));
  addStackPushG(sbOut, lastRef, freedVals, sv);
  if (PRINT_VALUE_MOVES){
    addPrintG2(lastRef,
               "Disowned last reference to %p! Freeing...\n", sv);
  }

  IRExpr* newRefCount =
    runBinop(sbOut, Iop_Sub64, prevRefCount, mkU64(1));
  if (PRINT_VALUE_MOVES){
    IRExpr* shouldPrintUpdate = runUnop(sbOut, Iop_Not1, lastRef);
    addPrintG3(shouldPrintUpdate,
               "[2] Disowning %p, new ref_count %d\n", sv, newRefCount);
  }
  addStoreArrow(sbOut, sv, ShadowValue,
                ref_count, newRefCount);
  addExprDisownG(sbOut, lastRef,
                 runArrowG(sbOut, lastRef, sv, ShadowValue, expr));
}
void addSVDisownNonNullG(IRSB* sbOut, IRExpr* guard, IRExpr* sv){
  IRExpr* prevRefCount =
    runArrowG(sbOut, guard, sv, ShadowValue, ref_count);
  IRExpr* lastRef = runBinop(sbOut, Iop_CmpEQ64, prevRefCount, mkU64(1));
  if (PRINT_VALUE_MOVES){
    addPrintG2(lastRef,
               "Disowned last reference to %p! Freeing...\n", sv);
  }
  addStackPushG(sbOut, lastRef, freedVals, sv);
  IRExpr* newRefCount =
    runBinop(sbOut, Iop_Sub64, prevRefCount, mkU64(1));
  if (PRINT_VALUE_MOVES){
    IRExpr* nonLastRef = runBinop(sbOut, Iop_CmpLT64U,
                                  mkU64(1), prevRefCount);
    addPrintG3(nonLastRef,
               "[3] Disowning %p, new ref_count %d\n", sv, newRefCount);
  }
  addStoreArrowG(sbOut, guard, sv, ShadowValue, ref_count, newRefCount);
  addExprDisownG(sbOut, lastRef,
                 runArrowG(sbOut, lastRef, sv, ShadowValue, expr));
}
void addSVDisownG(IRSB* sbOut, IRExpr* guard, IRExpr* sv){
  IRExpr* valueNonNull = runNonZeroCheck64(sbOut, sv);
  IRExpr* shouldDoAnythingAtAll = runAnd(sbOut, valueNonNull, guard);
  addSVDisownNonNullG(sbOut, shouldDoAnythingAtAll, sv);
}
void addExprDisownG(IRSB* sbOut, IRExpr* guard, IRExpr* expr){
  IRExpr* exprNonNull = runNonZeroCheck64(sbOut, expr);
  IRExpr* shouldDoAnything = runAnd(sbOut, guard, exprNonNull);
  IRExpr* prevRefCount =
    runArrowG(sbOut, shouldDoAnything, expr, ConcExpr, ref_count);
  IRExpr* newRefCount =
    runBinop(sbOut, Iop_Sub64, prevRefCount, mkU64(1));
  addStoreArrowG(sbOut, guard, expr, ConcExpr, ref_count, newRefCount);
  IRExpr* lastRef = runZeroCheck64(sbOut, newRefCount);
  IRExpr* isBranch =
    runBinop(sbOut, Iop_CmpEQ64,
             runArrowG(sbOut, shouldDoAnything,
                       expr, ConcExpr, type),
             mkU64(Node_Branch));
  addStackPushG(sbOut,
                runAnd(sbOut, lastRef,
                       runUnop(sbOut, Iop_Not1, isBranch)),
                leafCExprs, expr);
  IRExpr* shouldFreeBranch =
    runAnd(sbOut, isBranch, shouldDoAnything);
  IRStmt* freeBranch =
    mkDirtyG_0_1(freeBranchConcExpr, expr, shouldFreeBranch);
  addStmtToIRSB(sbOut, freeBranch);
}
void addClear(IRSB* sbOut, IRTemp dest, int num_vals){
  IRExpr* oldShadowTemp = runLoad64C(sbOut, &(shadowTemps[dest]));
  addDisownNonNull(sbOut, oldShadowTemp, num_vals);
  addStoreC(sbOut, mkU64(0), &(shadowTemps[dest]));
}
