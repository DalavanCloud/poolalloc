//===-- PoolAllocate.h - Pool allocation pass -------------------*- C++ -*-===//
//
// This transform changes programs so that disjoint data structures are
// allocated out of different pools of memory, increasing locality.  This header
// file exposes information about the pool allocation itself so that follow-on
// passes may extend or use the pool allocation for analysis.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_POOLALLOCATE_H
#define LLVM_TRANSFORMS_POOLALLOCATE_H

#include "llvm/Pass.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Support/CallSite.h"
#include "Support/hash_set"
#include "Support/VectorExtras.h"
#include "Support/EquivalenceClasses.h"
#include <set>

class BUDataStructures;
class TDDataStructures;
class DSNode;
class DSGraph;
class Type;
class AllocaInst;

namespace PA {
  /// FuncInfo - Represent the pool allocation information for one function in
  /// the program.  Note that many functions must actually be cloned in order
  /// for pool allocation to add arguments to the function signature.  In this
  /// case, the Clone and NewToOldValueMap information identify how the clone
  /// maps to the original function...
  ///
  struct FuncInfo {
    /// MarkedNodes - The set of nodes which are not locally pool allocatable in
    /// the current function.
    ///
    hash_set<DSNode*> MarkedNodes;

    /// Clone - The cloned version of the function, if applicable.
    Function *Clone;

    /// ArgNodes - The list of DSNodes which have pools passed in as arguments.
    /// 
    std::vector<DSNode*> ArgNodes;

    /// In order to handle indirect functions, the start and end of the 
    /// arguments that are useful to this function. 
    /// The pool arguments useful to this function are PoolArgFirst to 
    /// PoolArgLast not inclusive.
    int PoolArgFirst, PoolArgLast;
    
    /// PoolDescriptors - The Value* (either an argument or an alloca) which
    /// defines the pool descriptor for this DSNode.  Pools are mapped one to
    /// one with nodes in the DSGraph, so this contains a pointer to the node it
    /// corresponds to.  In addition, the pool is initialized by calling the
    /// "poolinit" library function with a chunk of memory allocated with an
    /// alloca instruction.  This entry contains a pointer to that alloca if the
    /// pool is locally allocated or the argument it is passed in through if
    /// not.
    /// Note: Does not include pool arguments that are passed in because of
    /// indirect function calls that are not used in the function.
    std::map<DSNode*, Value*> PoolDescriptors;

    /// NewToOldValueMap - When and if a function needs to be cloned, this map
    /// contains a mapping from all of the values in the new function back to
    /// the values they correspond to in the old function.
    ///
    std::map<Value*, const Value*> NewToOldValueMap;
  };
}



/// PoolAllocate - The main pool allocation pass
///
class PoolAllocate : public Pass {
  Module *CurModule;
  BUDataStructures *BU;

  TDDataStructures *TDDS;

  hash_set<Function*> InlinedFuncs;
  
  std::map<Function*, PA::FuncInfo> FunctionInfo;

  void buildIndirectFunctionSets(Module &M);   

  void FindFunctionPoolArgs(Function &F);   
  
  // Debug function to print the FuncECs
  void printFuncECs();
  
 public:

  Function *PoolInit, *PoolDestroy, *PoolAlloc, *PoolAllocArray, *PoolFree;
  static const Type *PoolDescPtrTy;

  // Equivalence class where functions that can potentially be called via
  // the same function pointer are in the same class.
  EquivalenceClasses<Function *> FuncECs;

  // Map from an Indirect call site to the set of Functions that it can point to
  std::multimap<CallSite, Function *> CallSiteTargets;

  // This maps an equivalence class to the last pool argument number for that 
  // class. This is used because the pool arguments for all functions within
  // an equivalence class is passed to all the functions in that class.
  // If an equivalence class does not require pool arguments, it is not
  // on this map.
  std::map<Function *, int> EqClass2LastPoolArg;

  // Exception flags
  // CollapseFlag set if all data structures are not pool allocated, due to
  // collapsing of nodes in the DS graph
  unsigned CollapseFlag;
  
 public:
  bool run(Module &M);
  
  virtual void getAnalysisUsage(AnalysisUsage &AU) const;
  
  BUDataStructures &getBUDataStructures() const { return *BU; }
  
  //Dinakar to get function info for all (cloned functions) 
  PA::FuncInfo *getFunctionInfo(Function *F) {
    //If it is cloned or not check it out
    if (FunctionInfo.count(F))
      return &FunctionInfo[F];
    else {
      //Probably cloned
      std::map<Function *, PA::FuncInfo>::iterator fI = FunctionInfo.begin(), 
	fE = FunctionInfo.end();
      for (; fI != fE; ++fI)
	if (fI->second.Clone == F)
	  return &fI->second;
      return 0;
    }
  }
  
  PA::FuncInfo *getFuncInfo(Function &F) {
    std::map<Function*, PA::FuncInfo>::iterator I = FunctionInfo.find(&F);
    return I != FunctionInfo.end() ? &I->second : 0;
  }

  Module *getCurModule() { return CurModule; }

 private:
  
  /// AddPoolPrototypes - Add prototypes for the pool functions to the
  /// specified module and update the Pool* instance variables to point to
  /// them.
  ///
  void AddPoolPrototypes();
  
  /// MakeFunctionClone - If the specified function needs to be modified for
  /// pool allocation support, make a clone of it, adding additional arguments
  /// as neccesary, and return it.  If not, just return null.
  ///
  Function *MakeFunctionClone(Function &F);
  
  /// ProcessFunctionBody - Rewrite the body of a transformed function to use
  /// pool allocation where appropriate.
  ///
  void ProcessFunctionBody(Function &Old, Function &New);
  
  /// CreatePools - This inserts alloca instruction in the function for all
  /// pools specified in the NodesToPA list.  This adds an entry to the
  /// PoolDescriptors map for each DSNode.
  ///
  void CreatePools(Function &F, const std::vector<DSNode*> &NodesToPA,
                   std::map<DSNode*, Value*> &PoolDescriptors);
  
  void InlineIndirectCalls(Function &F, DSGraph &G, 
			   hash_set<Function*> &visited);

  void TransformBody(DSGraph &g, DSGraph &tdg, PA::FuncInfo &fi,
                     std::set<std::pair<AllocaInst*, Instruction*> > &poolUses,
                     std::set<std::pair<AllocaInst*, CallInst*> > &poolFrees,
                     Function &F);

  /// InitializeAndDestroyPools - This inserts calls to poolinit and pooldestroy
  /// into the function to initialize and destroy the pools in the NodesToPA
  /// list.
  void InitializeAndDestroyPools(Function &F,
                                 const std::vector<DSNode*> &NodesToPA,
                                 std::map<DSNode*, Value*> &PoolDescriptors);
};

#endif
