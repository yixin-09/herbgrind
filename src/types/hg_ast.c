#include "hg_ast.h"
#include "hg_shadowvals.h"
#include "hg_opinfo.h"
#include "../include/hg_macros.h"
#include "../include/hg_options.h"

#include "pub_tool_libcprint.h"
#include "pub_tool_libcbase.h"
#include "pub_tool_mallocfree.h"
#include "pub_tool_libcassert.h"

#include <stdarg.h>

#define MAX_AST_STR_LEN 256

void initValueBranchAST(ShadowValue* val, Op_Info* opinfo,
                        SizeT nargs, ShadowValue* firstarg, ...){
  // Initialize the basic fields of an value AST, linking it to the
  // value it cooresponds to, and information on the op which it is
  // generated by.
  val->ast->val = val;
  val->ast->op = opinfo;

  // Link in the children of this node in the AST. Set the number of
  // children, nargs, to the passed value, and then allocate an array
  // of that size for pointers to the children.
  val->ast->nargs = nargs;
  ALLOC(val->ast->args, "hg.val_ast_args", nargs, sizeof(ShadowValue*));

  // Use the c variable arity mechanism to populate the array with the
  // passed pointers to children.
  va_list args;

  va_start(args, firstarg);

  val->ast->args[0] = firstarg->ast;
  addRef(firstarg);
  for(int i = 1; i < nargs; ++i){
    ShadowValue* newReference = NULL;
    copySV(va_arg(args, ShadowValue*), &newReference);
    val->ast->args[i] = newReference->ast;
  }
  va_end(args);

  // Finally, allocate a hash table to map leaf nodes to their
  // cooresponding variable indices. The actual index doesn't matter,
  // what matters is the grouping: leaves mapped to the same index are
  // the "same" variable in this trace.
  val->ast->var_map = VG_(HT_construct)("val_var_map");
  // Use initValVarMap to populate this table based on the arguments
  // we were passed.
  initValVarMap(val->ast);
}

void initValVarMap(ValueASTNode* valAST){
  // Build up a map from double values to variable indices, so that we
  // can identify when variable leaf nodes are the "same" variable. At
  // the same time, build up the var_map for our shadow value.
  VgHashTable* val_to_idx = VG_(HT_construct)("val_to_idx");
  int next_idx = 0;
  for (int i = 0; i < valAST->nargs; ++i){
    /// For each argument, we'll do one of two things to get all of
    /// it's leaf nodes and register them in our two maps.
    ValueASTNode* arg = valAST->args[i];
    if (arg->op->tag == Op_Leaf){
      // If the argument is already a leaf, then just register it in
      // both maps.
      registerLeaf(arg, &next_idx, val_to_idx, valAST->var_map);
    } else {
      // If the argument is a branch node (some sort of operation),
      // then it itself has a var_map with all of the leaf nodes under
      // it as keys. In this case, just iterate through the entries of
      // that var map, and pull out all the keys, and add them to our
      // map one at a time.
      VG_(HT_ResetIter)(arg->var_map);
      for (ValVarMapEntry* entry = VG_(HT_Next)(arg->var_map);
           entry != NULL; entry = VG_(HT_Next)(arg->var_map)){
        registerLeaf(entry->key, &next_idx, val_to_idx,
                     valAST->var_map);
      }
    }
  }
  // We only use the val_to_idx map to easily check for nodes that
  // have matching values in this trace, so that we can build up our
  // var map. We don't actually need it after we've built up the
  // var_map, so just destruct it now.
  VG_(HT_destruct)(val_to_idx, VG_(free));
}

void registerLeaf(ValueASTNode* leaf, int* idx_counter,
                  VgHashTable* val_to_idx, VgHashTable* var_map){
  // We're going to be matching leaf nodes based on the 32-bit float
  // version of their values. Since their high precision MPFR values
  // should have just been initialized from some float bits, and no
  // operations have yet been done on them since they are a leaf node,
  // this should be all we need to compare them, modulo the difference
  // between 32-bit and 64-bit. We use float instead of double because
  // otherwise on 32-bit platforms it might not fit in the key size
  // for the hash table. This means we're going to think some values
  // are the same that are actually slightly different, but only ones
  // that are always very similar, so hopefully that'll be fine.
  float val = mpfr_get_flt(leaf->val->value, MPFR_RNDN);
  // Lookup the value in our val_to_idx map to see if we've already
  // registered a leaf with the same double value. If so, we're going
  // to say that this leaf node and that one are the "same" variable
  // in the context of the current trace.

  // Make sure to reinterpret the float byte as a word, instead of
  // allowing c to do a semantic cast, since that would lose a bunch
  // of info.
  UWord valKey = 0;
  VG_(memcpy)(&valKey, &val, sizeof(float));

  ValMapEntry* val_entry = VG_(HT_lookup)(val_to_idx, valKey);
  // If there isn't an already existing entry with the same
  // value, then this is a value we haven't seen before, so
  // create a fresh variable index for it in val_to_idx.
  if (val_entry == NULL){
    // Add a new entry to our local map from values to
    // indices, so that next time we get a value that matches
    // this we'll map it to the same index.
    ALLOC(val_entry, "val_to_idx entry", 1, sizeof(ValMapEntry));
    // The key is the value of the leaf node, for future matching.
    val_entry->key = valKey;
    // The variable index will use the counter we were passed a
    // reference to so that we can keep it as state across calls to
    // registerLeaf within the same map building pass. Increment it
    // after we use it, so that we use fresh indices on each
    // registeration.
    val_entry->varidx = (*idx_counter)++;
    // Finally, add it to the map.
    VG_(HT_add_node)(val_to_idx, val_entry);
  }
  // Here we'll update our var_map for this op node, to map the leaf
  // node we're currently processing to a index which is unique to
  // it's value (but not necessarily the identity of this leaf).
  ValVarMapEntry* valvar_entry;
  ALLOC(valvar_entry, "leaf_to_idx entry", 1, sizeof(ValVarMapEntry));
  // The key is the location in memory of the leaf node.
  valvar_entry->key = leaf;
  // The value is the variable index that we it's value maps to. This
  // either already existed and we looked it up from a previous leaf
  // with the same value, or it didn't exist and we created it in the
  // previous if block.
  valvar_entry->varidx = val_entry->varidx;
  // Finally, add the entry.
  VG_(HT_add_node)(var_map, valvar_entry);
}

void initValueLeafAST(ShadowValue* val, Op_Info** src_loc){
  // Circular reference to the val. In some contexts we want to pass
  // just the AST to a function, but we actually want to mess around
  // with it's cooresponding value. There's probably a better way to
  // do this, but this doesn't seem too horrible yet.
  val->ast->val = val;

  // These fields really only apply to branch nodes, but instead of
  // being nice and clean and using a sum type (which has to be a
  // tagged union in c anyway... ugh), we're just going to set these
  // fields to 0 and NULL. This acts as an ad hoc tag too, since code
  // can check whether these are 0 or NULL to tell if the node in
  // question is a branch or leaf node. Branch nodes should always set
  // these to something non-zero.
  val->ast->nargs = 0;
  val->ast->args = NULL;
  val->ast->var_map = NULL;

  // Since this is a leaf value, meaning we didn't know it was even a
  // floating point value until now, we're going to say that it came
  // from some sort of "value source." We want to keep track of these
  // value sources so that we can associate instances of the "same"
  // variable in different parts of the program with each other. Each
  // branch op has slots within it for such value sources, so we ask
  // upon leaf value creation that the location of that slot is given
  // to us. If it holds null, then we didn't previously know this was
  // a leaf value, probably because this is the first time this
  // expression has been evaluated (although it could mean that the
  // area was recently abstracted into a value source variable, no
  // good story for what that means yet...). In that case, we'll
  // create a new source structure, an op leaf node for it. Otherwise
  // if the slot is already occupied, just link this new leaf value to
  // it.
  if (*src_loc == NULL)
    *src_loc = mkLeafOp_Info(val);
  val->ast->op = *src_loc;
}

// Cleanup the memory associated with the AST attached to the given
// value.
void cleanupValueAST(ShadowValue* val){
  if (val->ast->nargs != 0){
    // The var map needs to be specially destructed, since it's a hash
    // map.
    VG_(HT_destruct)(val->ast->var_map, VG_(free));
    // Release our references to the shadow values
    for (int i = 0; i < val->ast->nargs; ++i){
      disownSV(val->ast->args[i]->val);
    }
    // Free the argument array
    VG_(free)(val->ast->args);
  }
  // Free the ast directly
  VG_(free)(val->ast);
}

// Deep copy a value AST from one shadow value to another.
void copyValueAST(ShadowValue* src, ShadowValue* dest){
  ALLOC(dest->ast, "hg.val_ast", 1, sizeof(ValueASTNode));
  dest->ast->val = dest;
  dest->ast->op = src->ast->op;
  dest->ast->nargs = src->ast->nargs;
  dest->ast->var_map = src->ast->var_map;
  if (src->ast->nargs != 0){
    ALLOC(dest->ast->args, "hg.val_ast_args", src->ast->nargs, sizeof(ShadowValue*));
    for (int i = 0; i < src->ast->nargs; ++i){
      dest->ast->args[i] = src->ast->args[i];
      addRef(src->ast->args[i]->val);
    }
  }
}

// Initialize OpAST's, which are the persistent ast's that are
// generalized to fit every value ast that passes through them, and
// are reported at the end of the run.
void initOpBranchAST(OpASTNode* out, Op_Info* op, SizeT nargs){
  out->tag = Node_Branch;
  out->nd.Branch.op = op;
  out->nd.Branch.nargs = nargs;
  ALLOC(out->nd.Branch.args, "hg.val_ast_args", nargs, sizeof(OpASTNode*));
  out->nd.Branch.var_map = VG_(newXA)(VG_(malloc), "var_map",
                                      VG_(free), sizeof(XArray*));
}

void initOpLeafAST(OpASTNode* out, ShadowValue* val){
  out->tag = Node_Leaf;
  copySV(val, &(out->nd.Leaf.val));
}

void updateAST(Op_Info* op, ValueASTNode* trace_ast){
  if (op->ast == NULL){
    // The first time we see a value, the most specific AST that fits it
    // is exactly it's ast.
    op->ast = convertValASTtoOpAST(trace_ast);
  } else {
    // If we've already seen values, we'll want to generalize the AST
    // we have sufficiently to match the new value.
    generalizeAST(op->ast, trace_ast);
  }
  // This doesn't (shouldn't) affect the functionality of this
  // function, but allows us to print AST's on update, mostly for
  // debugging purposes. Or like, you might just be into that, in
  // which case more power to you.
  if (print_expr_updates){
    char* opASTString = opASTtoExpr(op->ast);
    VG_(printf)("Updating op ast to: %s\n", opASTString);
    VG_(free)(opASTString);
  }
}

void generalizeAST(OpASTNode* opast, ValueASTNode* valast){
  if (opast->tag == Node_Leaf){
    // If we hit a value leaf, and it matches the one we've already
    // seen, then our best guess right now is that that is a constant
    // which doesn't change in this expression, so leave it in our AST.
    if (valast->val == NULL ||
        opast->nd.Leaf.val == NULL ||
        mpfr_get_d(valast->val->value, MPFR_RNDN)
        == mpfr_get_d(opast->nd.Leaf.val->value, MPFR_RNDN))
      return;
    // Otherwise, it's some sort of input that changes, so abstract it
    // into a variable by setting it's val field to NULL.
    else {
      copySV(NULL, &(opast->nd.Leaf.val));
      return;
    }
  } else {
    // We're at a branch node.
    if (valast->op != opast->nd.Branch.op){
      // If the valast is a leaf node, or it continues but it doesn't
      // match the opast, cut off the opast here, with a variable leaf
      // node (one where the shadow value is NULL, because we've seen
      // different values here).

      // Once we free the args array we malloc'ed, we can just
      // overwrite everything with new initial values, and things will
      // probably turn out fine.
      VG_(free)(opast->nd.Branch.args);
      initOpLeafAST(opast, NULL);
    } else if (opast->nd.Branch.op != NULL) {
      // Otherwise, if they both continue and match, generalize the
      // variable map appropriately, and recurse on children,
      generalizeVarMap(opast->nd.Branch.var_map, valast->var_map);
      for(int i = 0; i < valast->nargs; ++i){
        generalizeAST(opast->nd.Branch.args[i], valast->args[i]);
      }
    }
  }
}

OpASTNode* convertValASTtoOpAST(ValueASTNode* valAST){
  // First, check if we've already made an AST for the op that this
  // value came from. If so, just share that.
  if (valAST->op->ast != NULL) {
    return valAST->op->ast;
  }

  // Otherwise, we need to create a new AST node. It'll be a branch,
  // since leaves start initialized.
  OpASTNode* result;
  ALLOC(result, "hg.op_ast", 1, sizeof(OpASTNode));
  initOpBranchAST(result, valAST->op, valAST->nargs);
  // Convert all of the children recursively. Usually they won't hit
  // this branch again, since we generally build subexpression AST's
  // before their parents, with the exception of leaf nodes since
  // they don't get build until a branch needs them. So, our
  // children should either already have an op ast somewhere, or
  // they are leaf nodes. This isn't necessarily an invariant I'm
  // commited to, so we should operate fine if it's not true, but
  // for clarity's sake that is what I currently expect.
  for (int i = 0; i < valAST->nargs; ++i){
    result->nd.Branch.args[i] = convertValASTtoOpAST(valAST->args[i]);
  }

  // Finally, since this is the first value map this op has seen, it
  // copies it as it's own this means that any values that were the
  // same this time are currently assumed to be the same variable
  // (or constant), and anything that isn't the same this time can
  // never be the same variable.
  result->nd.Branch.var_map = opvarmapFromValvarmap(valAST->var_map);

  // Set the ast field of the op info so that next time we can reuse
  // what we've created.
  valAST->op->ast = result;

  return result;
}


// These two things are for debugging the variable matching code.
void printLookupTable(VgHashTable* opLookupTable){
  VG_(HT_ResetIter)(opLookupTable);
  VG_(printf)("==================================\n");
  for(OpVarMapEntry* entry = VG_(HT_Next)(opLookupTable);
      entry != NULL; entry = VG_(HT_Next)(opLookupTable)){
    VG_(printf)("%p -> %d\n", entry->key, entry->varidx);
  }
  VG_(printf)("\n");
}

void printOpVarMap(XArray* opVarMap){
  for(int i = 0; i < VG_(sizeXA)(opVarMap); ++i){
    XArray** varGroupEntry = VG_(indexXA)(opVarMap, i);
    for (int j = 0; j < VG_(sizeXA)(*varGroupEntry); ++j){
      OpASTNode** nodeEntry = VG_(indexXA)(*varGroupEntry, j);
      VG_(printf)("%p, ", *nodeEntry);
    }
    VG_(printf)("\n");
  }
  VG_(printf)("\n");
}

XArray* opvarmapFromValvarmap(VgHashTable* valVarMap){
  XArray* opVarMap = VG_(newXA)(VG_(malloc), "opVarMap",
                                VG_(free), sizeof(XArray*));
  // Go through the entries of the value var map
  VG_(HT_ResetIter)(valVarMap);
  for (ValVarMapEntry* valEntry = VG_(HT_Next)(valVarMap);
       valEntry != NULL; valEntry = VG_(HT_Next)(valVarMap)){
    // If it has an index higher than any one we've seen so far,
    // create empty groups up through that index.
    while(VG_(sizeXA)(opVarMap) <= valEntry->varidx){
      XArray* newVarGroup = VG_(newXA)(VG_(malloc), "opVarMapRow",
                                        VG_(free), sizeof(OpASTNode*));
      VG_(addToXA)(opVarMap, &newVarGroup);
    }
    // Extract the group that matches the index that our var map maps
    // to.
    XArray** entry = VG_(indexXA)(opVarMap, valEntry->varidx);
    XArray* varGroup = *entry;
    // Convert the key to it's equivalent op node.
    OpASTNode* opNode = convertValASTtoOpAST(valEntry->key);
    // Add the op node to the group that it's key maps to.
    VG_(addToXA)(varGroup, &opNode);
  }
  return opVarMap;
}

VgHashTable* opLookupTable(VgHashTable* valVarMap){
  VgHashTable* lookupTable = VG_(HT_construct)("lookupTable");
  // Go through the entries...
  VG_(HT_ResetIter)(valVarMap);
  for (ValVarMapEntry* valEntry = VG_(HT_Next)(valVarMap);
       valEntry != NULL; valEntry = VG_(HT_Next)(valVarMap)){
    OpVarMapEntry* opEntry;
    ALLOC(opEntry, "opEntry", 1, sizeof(OpVarMapEntry));
    // Get the cooresponding key
    opEntry->key = convertValASTtoOpAST(valEntry->key);
    // Map it to the same value
    opEntry->varidx = valEntry->varidx;
    VG_(HT_add_node)(lookupTable, opEntry);
  }
  return lookupTable;
}

typedef struct _IdxMapEntry {
  struct _IdxMapEntry* next;
  UWord key;
  int val;
} IdxMapEntry;

Bool inOpVarMap(XArray* haystack, OpASTNode* needle);
Bool inOpVarMap(XArray* haystack, OpASTNode* needle){
  for (int i = 0; i < VG_(sizeXA)(haystack); ++i){
    XArray** varGroupEntry = VG_(indexXA)(haystack, i);
    for (int j = 0; j < VG_(sizeXA)(*varGroupEntry); ++j){
      OpASTNode** cellEntry = VG_(indexXA)(*varGroupEntry, j);
      if ((*cellEntry) == needle){
        return True;
      }
    }
  }
  return False;
}

// This is an invariant checker, for debugging.  It checks the
// following invariant: the set of keys in the valueLookupTable and
// the items in the two dimentonal array opVarMap should match.
void checkOpVarMapValueLookupTableMatch(XArray* opVarMap, VgHashTable* valueLookupTable);
void checkOpVarMapValueLookupTableMatch(XArray* opVarMap, VgHashTable* valueLookupTable){
  for (int i = 0; i < VG_(sizeXA)(opVarMap); ++i){
    XArray** varGroupEntry = VG_(indexXA)(opVarMap, i);
    for (int j = 0; j < VG_(sizeXA)(*varGroupEntry); ++j){
      OpASTNode** cellEntry = VG_(indexXA)(*varGroupEntry, j);
      OpVarMapEntry* valOpEntry =
        VG_(HT_lookup)(valueLookupTable, (UWord)*cellEntry);
      tl_assert(valOpEntry != NULL);
    }
  }
  VG_(printf)("Lookup table: \n");
  printLookupTable(valueLookupTable);
  VG_(printf)("opVarMap: \n");
  printOpVarMap(opVarMap);
  VG_(HT_ResetIter)(valueLookupTable);
  for (OpVarMapEntry* opEntry = VG_(HT_Next)(valueLookupTable);
       opEntry != NULL; opEntry = VG_(HT_Next)(valueLookupTable)){
    tl_assert(inOpVarMap(opVarMap, opEntry->key));
  }
}

void checkOpVarMapValVarMapSameLeaves(XArray* opVarMap, VgHashTable* valVarMap){
  VgHashTable* valueLookupTable = opLookupTable(valVarMap);
  checkOpVarMapValueLookupTableMatch(opVarMap, valueLookupTable);
}

// The purpose of this function is to take an existing variable map
// from an op node, and generalize it using a trace variable map tied
// to a particular shadow value ast. This generalization should be
// such that no two leaves which are mapped to different variable
// indices in the trace are mapped to the same variable index in the
// result, but other than that the op node map stays as unchanged as
// possible.
void generalizeVarMap(XArray* opVarMap, VgHashTable* valVarMap){
  // The first thing we need to do is take our trace var map, which
  // maps trace leaves to variable indices, and get a map from op AST
  // leaves to the same variable indices. This way, we'll be comparing
  // apples to apples when we use the resulting map to generalize our
  // opVarMap, which also talks about op AST leaves.
  VgHashTable* valueLookupTable = opLookupTable(valVarMap);
  checkOpVarMapValueLookupTableMatch(opVarMap, valueLookupTable);
  VG_(printf)("Got past this at least once.\n");
  // There's no point trying to re-split the groups we split off,
  // since our procedure should make them already consistent with the
  // valVarMap, so get the size once, and don't touch the entries that
  // are added later.
  int initialOpVarMapSize = VG_(sizeXA)(opVarMap);
  // Now, let's iterate over the variable index groups in our opVarMap.
  for (int i = 0; i < initialOpVarMapSize; ++i){
    // Lookup the variable index group for i. This get's us a pointer
    // to the location in the XArray where that variable index group
    // resides.
    XArray** varGroupEntry = VG_(indexXA)(opVarMap, i);
    // For each group of leaves that currently map to the same
    // variable index, we're going to want to split off any leaves
    // that don't map to the same index as the rest in valVarMap,
    // accomplishing the target generalization of our opVarMap. To do
    // this, we'll figure out what the first leaf in the group maps to
    // in valVarMap, and then split off other leaves which don't match
    // that. But, if we have leaves in our group mapping to [1, 2, 2],
    // we want to make sure that the two 2's get split off into the
    // SAME group. So, we'll maintain a map from indices in valVarMap
    // to indices of groups that they split to. This is the split map.
    VgHashTable* splitMap = VG_(HT_construct)("splitMap");
    // Get a pointer to the first leaf pointer in the group.
    OpASTNode** firstNodeEntry = VG_(indexXA)(*varGroupEntry, 0);
    // Now, map this first leaf pointer to the initial group in the
    // split map. This means that any future element of the var group
    // which matches, the first element will stay in the var group,
    // and not get split off, which is what we want.

    // To do this, first lookup the valVarMap index of the first
    // element.
    OpVarMapEntry* valOpEntry =
      // Need to dereference to get the OpASTNode*, and then cast that
      // to UWord for hash table lookup
      VG_(HT_lookup)(valueLookupTable, (UWord)*firstNodeEntry);
    // Allocate a new entry for this initial splitmap 
    IdxMapEntry* splitEntry;
    ALLOC(splitEntry, "splitEntry", 1, sizeof(IdxMapEntry));
    if (valOpEntry == NULL){
      VG_(printf)("hey!!! i is %d\n", i);
      printLookupTable(valueLookupTable);
      VG_(printf)("varGroupEntry is %p\n", varGroupEntry);
      printOpVarMap(opVarMap);
      VG_(printf)("Tried to look up key: %p\n", (void*)(UWord)*firstNodeEntry);
    }
    // The key is the first elements varidx.
    splitEntry->key = valOpEntry->varidx;
    // The value is the current group.
    splitEntry->val = i;
    // Add the entry.
    VG_(HT_add_node)(splitMap, splitEntry);
    // Now, loop through the rest of the entries, splitting them off
    // as needed.
    for(int j = 1; j < VG_(sizeXA)(*varGroupEntry); ++j){
      // Pull out the next element in the var group.
      OpASTNode* curNode = *(OpASTNode**)VG_(indexXA)(*varGroupEntry, j);
      // Lookup it's index in the trace map.
      valOpEntry = VG_(HT_lookup)(valueLookupTable, (UWord)curNode);
      // Lookup in the split map to see if that matched an earlier index.
      splitEntry = VG_(HT_lookup)(splitMap, valOpEntry->varidx);
      // If not...
      if (splitEntry == NULL){
        // This means this leaf doesn't match earlier ones in this
        // varGroup, so we need to create a new group for it, and
        // remove it from the old one.
        XArray* newGroup = (VG_(newXA)(VG_(malloc), "varGroup",
                                       VG_(free), sizeof(XArray*)));
        VG_(addToXA)(newGroup, &curNode);
        Word newIndex = VG_(addToXA)(opVarMap, &newGroup);
        // After we insert into the opVarMap array, it might outgrow
        // it's currently allocated memory, and have to move. If this
        // happens, our varGroupEntry pointer will be invalidated, so
        // let's give it a quick refresh.
        varGroupEntry = VG_(indexXA)(opVarMap, i);

        // Also, we need to set up a new splitmap entry so that future
        // matching leaves get redirected to this var group.
        ALLOC(splitEntry, "splitEntry", 1, sizeof(IdxMapEntry));
        splitEntry->key = valOpEntry->varidx;
        splitEntry->val = newIndex;
        VG_(HT_add_node)(splitMap, splitEntry);

        // Remove the old group entry
        VG_(removeIndexXA)(*varGroupEntry, j--);
      } else if (splitEntry->val != i) {
        // If we already have an entry in the split map for it, but it
        // doesn't map to the current group, move it to the correct
        // group. This means that we split off a previous entry in
        // this group into it's own group, and this current entry
        // matches that one.
        VG_(addToXA)(*(XArray**)VG_(indexXA)(opVarMap, splitEntry->val), &curNode);
        VG_(removeIndexXA)(*varGroupEntry, j--);
      }
    }
    // We only need the split map for splitting individual groups, as
    // we don't want entries from different groups in our initial
    // opVarMap to end up mapped to the same group in the result.
    VG_(HT_destruct)(splitMap, VG_(free));
  }
  // Finally, we're done with this thing, so we can remove it.
  VG_(HT_destruct)(valueLookupTable, VG_(free));
}

// Take a map from indices to groups of op nodes, and flip it to a map
// from each op node to the index which represented its group.
VgHashTable* flipOpVarMap(XArray* opVarMap){
  VgHashTable* result = VG_(HT_construct)("flippedOpVarMap");
  // Go through the rows and columns of the opVarMap...
  for (int i = 0; i < VG_(sizeXA)(opVarMap); ++i){
    XArray** rowEntry = VG_(indexXA)(opVarMap, i);
    for (int j = 0; j < VG_(sizeXA)(*rowEntry); ++j){
      OpASTNode** entry = VG_(indexXA)(*rowEntry, j);
      // For each one, map it to it's row number in the resulting map.
      OpVarMapEntry* mapEntry;
      ALLOC(mapEntry, "opMapEntry", 1, sizeof(OpVarMapEntry));
      mapEntry->key = *entry;
      mapEntry->varidx = i;
      VG_(HT_add_node)(result, mapEntry);
    }
  }
  return result;
}

static const char* varNames[8] = {"x", "y", "z", "w", "a", "b", "c", "d"};

Bool inXArray(XArray* haystack, int needle){
  for(int i = 0; i < VG_(sizeXA)(haystack); ++i){
    if (*(int*)VG_(indexXA)(haystack, i) == needle)
      return True;
  }
  return False;
}

// Internal
void getUsedIndices(XArray* acc, OpASTNode* opAST, VgHashTable* varMap);
void getUsedIndices(XArray* acc, OpASTNode* opAST, VgHashTable* varMap){
  if (opAST->tag == Node_Leaf){
    if (opAST->nd.Leaf.val == NULL){      // If it's a variable and not a constant...
      OpVarMapEntry* varIdxEntry = VG_(HT_lookup)(varMap, (UWord)opAST);
      if (varIdxEntry == NULL){
          VG_(printf)("Problem! Couldn't find entry for leaf node in var map.\n");
      } else {
        int varIdx = varIdxEntry->varidx;
        if (!inXArray(acc, varIdx)){
          VG_(addToXA)(acc, &varIdx);
        }
      }
    }
  } else {
    // Walk down the tree recursively, updating our acc array.
    for (SizeT i = 0; i < opAST->nd.Branch.nargs; ++i){
      getUsedIndices(acc, opAST->nd.Branch.args[i], varMap);
    }
  }
}

// Given an AST (as the node at the top of one), returns all the
// variables bound in that ast.
XArray* usedVars(OpASTNode* opAST){
  XArray* usedVars = VG_(newXA)(VG_(malloc), "used_vars",
                                VG_(free), sizeof(char*));
  if (opAST->tag == Node_Leaf){
    if (opAST->nd.Leaf.val == NULL){
      VG_(addToXA)(usedVars, &(varNames[0]));
    }
    return usedVars;
  }
  XArray* usedIndices = VG_(newXA)(VG_(malloc), "used_indices",
                                   VG_(free), sizeof(int));
  getUsedIndices(usedIndices, opAST, flipOpVarMap(opAST->nd.Branch.var_map));
  for(int i = 0; i < VG_(sizeXA)(usedIndices); ++i){
    VG_(addToXA)(usedVars, &(varNames[*(int*)VG_(indexXA)(usedIndices, i)]));
  }
  VG_(deleteXA)(usedIndices);
  return usedVars;
}

// Give a printed representation of an op ast.
char* opASTtoExpr(OpASTNode* opAST){
  // If we're trying to print a leaf node, then we don't have any map
  // to label variables. In this case, pass NULL to the inner
  // function, opASTtoStringwithVarMap, and we'll just print it as "x".
  VgHashTable* map = NULL;
  if (opAST->tag == Node_Branch){
    // If it's a branch, take the representation which was most
    // convenient for building up the maps, and flip it around so that
    // it'll be useful for printing.
    map = flipOpVarMap(opAST->nd.Branch.var_map);
  }
  // Get the resulting string we're going to return.
  char* result = opASTtoExprwithVarMap(opAST, map);
  // If we made a flipped map, free it up now.
  if (map != NULL)
    VG_(HT_destruct)(map, VG_(free));
  // Return the representation.
  return result;
}
// This is a crude and wasteful function, but hopefully no one will
// notice.
char* opASTtoExprwithVarMap(OpASTNode* opAST, VgHashTable* varMap){
  char* buf;
  // This is our "cursor" in the output string.
  SizeT bufpos = 0;
  ALLOC(buf, "hg.ast_string", MAX_AST_STR_LEN, sizeof(char));

  // If this is a leaf node...
  if (opAST->tag == Node_Leaf){
    // It could either be a constant or a variable. 
    if (opAST->nd.Leaf.val == NULL){
      // If we're straight printing a leaf without any context, varMap
      // will be passed as null, so let's just print the first
      // variable name we can find.
      if (varMap == NULL)
        VG_(snprintf)(buf, 2, "%s", varNames[0]);
      else {
        // Otherwise, try to look up the leaf in our varmap.
        OpVarMapEntry* varIdxEntry = VG_(HT_lookup)(varMap, (UWord)opAST);
        if (varIdxEntry == NULL){
          // This should never happen.
          VG_(printf)("Problem! Couldn't find entry for leaf node in var map.\n");
          VG_(snprintf)(buf, 4, "XXX");
        } else
          VG_(snprintf)(buf, 2, "%s", varNames[varIdxEntry->varidx]);
      }
    } else
      // For constants we'll get the double value associated with the
      // constant, and print it.
      VG_(snprintf)(buf, MAX_AST_STR_LEN, "%f",
                    mpfr_get_d(opAST->nd.Leaf.val->value, MPFR_RNDN));
  } else {
    // Print the opening parenthesis and operator.
    bufpos += VG_(snprintf)(buf + bufpos, MAX_AST_STR_LEN - bufpos, "(%s",
                            opAST->nd.Branch.op->debuginfo.symbol);
    // Recursively get the subexpression strings, and print them
    // preceded by a space.
    for (SizeT i = 0; i < opAST->nd.Branch.nargs; ++i){
      char* subexpr = opASTtoExprwithVarMap(opAST->nd.Branch.args[i], varMap);
      bufpos += VG_(snprintf)(buf + bufpos, MAX_AST_STR_LEN - bufpos, " %s",
                              subexpr);
      VG_(free)(subexpr);
    }
    // Finally, print the closing parenthesis.
    bufpos += VG_(snprintf)(buf + bufpos, MAX_AST_STR_LEN - bufpos, ")");
  }
  return buf;
}
char* opASTtoBench(OpASTNode* opAST){
  XArray* vars = usedVars(opAST);
  char* binderString;
  // We're assuming here that each variable is only one character long
  // to size this allocation.
  SizeT binderStringSize = (VG_(sizeXA)(vars) * 2);
  // No matter what we need enough space for the null character
  if (binderStringSize == 0)
    binderStringSize = 1;

  ALLOC(binderString, "hg.binder_string", sizeof(char), binderStringSize);

  SizeT cursor = 0;
  for (int i = 0; i < VG_(sizeXA)(vars); ++i){
    // Same assumption again.
    binderString[cursor++] = (*(char**)VG_(indexXA)(vars, i))[0];
    if (i < VG_(sizeXA)(vars) - 1)
      binderString[cursor++] = ' ';
  }
  char* exprString = opASTtoExpr(opAST);
  SizeT exprStringSize = VG_(strlen)(exprString);
  SizeT benchStringSize =
    9 /* "(FPCore (" */ + binderStringSize - 1 /* This one includes a null char which we don't need */ +
    23 /* ")\n  :type binary64\n  " */ + exprStringSize +
    2 /* ")\0" */;
  char* benchString;
  ALLOC(benchString, "hg.bench_string", sizeof(char), benchStringSize);
  VG_(snprintf)(benchString, benchStringSize,
                "(FPCore (%s)\n  :type binary64\n  %s)",
                binderString,
                exprString);
  VG_(free)(binderString);
  VG_(free)(exprString);
  return benchString;
}
