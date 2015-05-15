//===-- GlobalMerge.cpp - Internal globals merging  -----------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
// This pass merges globals with internal linkage into one. This way all the
// globals which were merged into a biggest one can be addressed using offsets
// from the same base pointer (no need for separate base pointer for each of the
// global). Such a transformation can significantly reduce the register pressure
// when many globals are involved.
//
// For example, consider the code which touches several global variables at
// once:
//
// static int foo[N], bar[N], baz[N];
//
// for (i = 0; i < N; ++i) {
//    foo[i] = bar[i] * baz[i];
// }
//
//  On ARM the addresses of 3 arrays should be kept in the registers, thus
//  this code has quite large register pressure (loop body):
//
//  ldr     r1, [r5], #4
//  ldr     r2, [r6], #4
//  mul     r1, r2, r1
//  str     r1, [r0], #4
//
//  Pass converts the code to something like:
//
//  static struct {
//    int foo[N];
//    int bar[N];
//    int baz[N];
//  } merged;
//
//  for (i = 0; i < N; ++i) {
//    merged.foo[i] = merged.bar[i] * merged.baz[i];
//  }
//
//  and in ARM code this becomes:
//
//  ldr     r0, [r5, #40]
//  ldr     r1, [r5, #80]
//  mul     r0, r1, r0
//  str     r0, [r5], #4
//
//  note that we saved 2 registers here almostly "for free".
// ===---------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Target/TargetLowering.h"
#include "llvm/Target/TargetLoweringObjectFile.h"
#include "llvm/Target/TargetSubtargetInfo.h"
using namespace llvm;

#define DEBUG_TYPE "global-merge"

// FIXME: This is only useful as a last-resort way to disable the pass.
cl::opt<bool>
EnableGlobalMerge("enable-global-merge", cl::Hidden,
                  cl::desc("Enable the global merge pass"),
                  cl::init(true));

static cl::opt<bool>
EnableGlobalMergeOnConst("global-merge-on-const", cl::Hidden,
                         cl::desc("Enable global merge pass on constants"),
                         cl::init(false));

// FIXME: this could be a transitional option, and we probably need to remove
// it if only we are sure this optimization could always benefit all targets.
static cl::opt<bool>
EnableGlobalMergeOnExternal("global-merge-on-external", cl::Hidden,
     cl::desc("Enable global merge pass on external linkage"),
     cl::init(false));

STATISTIC(NumMerged, "Number of globals merged");
namespace {
  class GlobalMerge : public FunctionPass {
    const TargetMachine *TM;
    const DataLayout *DL;
    // FIXME: Infer the maximum possible offset depending on the actual users
    // (these max offsets are different for the users inside Thumb or ARM
    // functions), see the code that passes in the offset in the ARM backend
    // for more information.
    unsigned MaxOffset;

    bool doMerge(SmallVectorImpl<GlobalVariable*> &Globals,
                 Module &M, bool isConst, unsigned AddrSpace) const;

    /// \brief Check if the given variable has been identified as must keep
    /// \pre setMustKeepGlobalVariables must have been called on the Module that
    ///      contains GV
    bool isMustKeepGlobalVariable(const GlobalVariable *GV) const {
      return MustKeepGlobalVariables.count(GV);
    }

    /// Collect every variables marked as "used" or used in a landing pad
    /// instruction for this Module.
    void setMustKeepGlobalVariables(Module &M);

    /// Collect every variables marked as "used"
    void collectUsedGlobalVariables(Module &M);

    /// Keep track of the GlobalVariable that must not be merged away
    SmallPtrSet<const GlobalVariable *, 16> MustKeepGlobalVariables;

  public:
    static char ID;             // Pass identification, replacement for typeid.
    explicit GlobalMerge(const TargetMachine *TM = nullptr,
                         unsigned MaximalOffset = 0)
        : FunctionPass(ID), TM(TM), DL(TM->getDataLayout()),
          MaxOffset(MaximalOffset) {
      initializeGlobalMergePass(*PassRegistry::getPassRegistry());
    }

    bool doInitialization(Module &M) override;
    bool runOnFunction(Function &F) override;
    bool doFinalization(Module &M) override;

    const char *getPassName() const override {
      return "Merge internal globals";
    }

    void getAnalysisUsage(AnalysisUsage &AU) const override {
      AU.setPreservesCFG();
      FunctionPass::getAnalysisUsage(AU);
    }
  };
} // end anonymous namespace

char GlobalMerge::ID = 0;
INITIALIZE_PASS_BEGIN(GlobalMerge, "global-merge", "Merge global variables",
                      false, false)
INITIALIZE_PASS_END(GlobalMerge, "global-merge", "Merge global variables",
                    false, false)

bool GlobalMerge::doMerge(SmallVectorImpl<GlobalVariable*> &Globals,
                          Module &M, bool isConst, unsigned AddrSpace) const {
  // FIXME: Find better heuristics
  std::stable_sort(Globals.begin(), Globals.end(),
                   [this](const GlobalVariable *GV1, const GlobalVariable *GV2) {
    Type *Ty1 = cast<PointerType>(GV1->getType())->getElementType();
    Type *Ty2 = cast<PointerType>(GV2->getType())->getElementType();

    return (DL->getTypeAllocSize(Ty1) < DL->getTypeAllocSize(Ty2));
  });

  Type *Int32Ty = Type::getInt32Ty(M.getContext());

  assert(Globals.size() > 1);

  // FIXME: This simple solution merges globals all together as maximum as
  // possible. However, with this solution it would be hard to remove dead
  // global symbols at link-time. An alternative solution could be checking
  // global symbols references function by function, and make the symbols
  // being referred in the same function merged and we would probably need
  // to introduce heuristic algorithm to solve the merge conflict from
  // different functions.
  for (size_t i = 0, e = Globals.size(); i != e; ) {
    size_t j = 0;
    uint64_t MergedSize = 0;
    std::vector<Type*> Tys;
    std::vector<Constant*> Inits;

    bool HasExternal = false;
    GlobalVariable *TheFirstExternal = 0;
    for (j = i; j != e; ++j) {
      Type *Ty = Globals[j]->getType()->getElementType();
      MergedSize += DL->getTypeAllocSize(Ty);
      if (MergedSize > MaxOffset) {
        break;
      }
      Tys.push_back(Ty);
      Inits.push_back(Globals[j]->getInitializer());

      if (Globals[j]->hasExternalLinkage() && !HasExternal) {
        HasExternal = true;
        TheFirstExternal = Globals[j];
      }
    }

    // If merged variables doesn't have external linkage, we needn't to expose
    // the symbol after merging.
    GlobalValue::LinkageTypes Linkage = HasExternal
                                            ? GlobalValue::ExternalLinkage
                                            : GlobalValue::InternalLinkage;

    StructType *MergedTy = StructType::get(M.getContext(), Tys);
    Constant *MergedInit = ConstantStruct::get(MergedTy, Inits);

    // If merged variables have external linkage, we use symbol name of the
    // first variable merged as the suffix of global symbol name. This would
    // be able to avoid the link-time naming conflict for globalm symbols.
    GlobalVariable *MergedGV = new GlobalVariable(
        M, MergedTy, isConst, Linkage, MergedInit,
        HasExternal ? "_MergedGlobals_" + TheFirstExternal->getName()
                    : "_MergedGlobals",
        nullptr, GlobalVariable::NotThreadLocal, AddrSpace);

    for (size_t k = i; k < j; ++k) {
      GlobalValue::LinkageTypes Linkage = Globals[k]->getLinkage();
      std::string Name = Globals[k]->getName();

      Constant *Idx[2] = {
        ConstantInt::get(Int32Ty, 0),
        ConstantInt::get(Int32Ty, k-i)
      };
      Constant *GEP =
          ConstantExpr::getInBoundsGetElementPtr(MergedTy, MergedGV, Idx);
      Globals[k]->replaceAllUsesWith(GEP);
      Globals[k]->eraseFromParent();

      if (Linkage != GlobalValue::InternalLinkage) {
        // Generate a new alias...
        auto *PTy = cast<PointerType>(GEP->getType());
        GlobalAlias::create(PTy->getElementType(), PTy->getAddressSpace(),
                            Linkage, Name, GEP, &M);
      }

      NumMerged++;
    }
    i = j;
  }

  return true;
}

void GlobalMerge::collectUsedGlobalVariables(Module &M) {
  // Extract global variables from llvm.used array
  const GlobalVariable *GV = M.getGlobalVariable("llvm.used");
  if (!GV || !GV->hasInitializer()) return;

  // Should be an array of 'i8*'.
  const ConstantArray *InitList = cast<ConstantArray>(GV->getInitializer());

  for (unsigned i = 0, e = InitList->getNumOperands(); i != e; ++i)
    if (const GlobalVariable *G =
        dyn_cast<GlobalVariable>(InitList->getOperand(i)->stripPointerCasts()))
      MustKeepGlobalVariables.insert(G);
}

void GlobalMerge::setMustKeepGlobalVariables(Module &M) {
  collectUsedGlobalVariables(M);

  for (Module::iterator IFn = M.begin(), IEndFn = M.end(); IFn != IEndFn;
       ++IFn) {
    for (Function::iterator IBB = IFn->begin(), IEndBB = IFn->end();
         IBB != IEndBB; ++IBB) {
      // Follow the invoke link to find the landing pad instruction
      const InvokeInst *II = dyn_cast<InvokeInst>(IBB->getTerminator());
      if (!II) continue;

      const LandingPadInst *LPInst = II->getUnwindDest()->getLandingPadInst();
      // Look for globals in the clauses of the landing pad instruction
      for (unsigned Idx = 0, NumClauses = LPInst->getNumClauses();
           Idx != NumClauses; ++Idx)
        if (const GlobalVariable *GV =
            dyn_cast<GlobalVariable>(LPInst->getClause(Idx)
                                     ->stripPointerCasts()))
          MustKeepGlobalVariables.insert(GV);
    }
  }
}

bool GlobalMerge::doInitialization(Module &M) {
  if (!EnableGlobalMerge)
    return false;

  DenseMap<unsigned, SmallVector<GlobalVariable*, 16> > Globals, ConstGlobals,
                                                        BSSGlobals;
  bool Changed = false;
  setMustKeepGlobalVariables(M);

  // Grab all non-const globals.
  for (Module::global_iterator I = M.global_begin(),
         E = M.global_end(); I != E; ++I) {
    // Merge is safe for "normal" internal or external globals only
    if (I->isDeclaration() || I->isThreadLocal() || I->hasSection())
      continue;

    if (!(EnableGlobalMergeOnExternal && I->hasExternalLinkage()) &&
        !I->hasInternalLinkage())
      continue;

    PointerType *PT = dyn_cast<PointerType>(I->getType());
    assert(PT && "Global variable is not a pointer!");

    unsigned AddressSpace = PT->getAddressSpace();

    // Ignore fancy-aligned globals for now.
    unsigned Alignment = DL->getPreferredAlignment(I);
    Type *Ty = I->getType()->getElementType();
    if (Alignment > DL->getABITypeAlignment(Ty))
      continue;

    // Ignore all 'special' globals.
    if (I->getName().startswith("llvm.") ||
        I->getName().startswith(".llvm."))
      continue;

    // Ignore all "required" globals:
    if (isMustKeepGlobalVariable(I))
      continue;

    if (DL->getTypeAllocSize(Ty) < MaxOffset) {
      if (TargetLoweringObjectFile::getKindForGlobal(I, *TM).isBSSLocal())
        BSSGlobals[AddressSpace].push_back(I);
      else if (I->isConstant())
        ConstGlobals[AddressSpace].push_back(I);
      else
        Globals[AddressSpace].push_back(I);
    }
  }

  for (DenseMap<unsigned, SmallVector<GlobalVariable*, 16> >::iterator
       I = Globals.begin(), E = Globals.end(); I != E; ++I)
    if (I->second.size() > 1)
      Changed |= doMerge(I->second, M, false, I->first);

  for (DenseMap<unsigned, SmallVector<GlobalVariable*, 16> >::iterator
       I = BSSGlobals.begin(), E = BSSGlobals.end(); I != E; ++I)
    if (I->second.size() > 1)
      Changed |= doMerge(I->second, M, false, I->first);

  if (EnableGlobalMergeOnConst)
    for (DenseMap<unsigned, SmallVector<GlobalVariable*, 16> >::iterator
         I = ConstGlobals.begin(), E = ConstGlobals.end(); I != E; ++I)
      if (I->second.size() > 1)
        Changed |= doMerge(I->second, M, true, I->first);

  return Changed;
}

bool GlobalMerge::runOnFunction(Function &F) {
  return false;
}

bool GlobalMerge::doFinalization(Module &M) {
  MustKeepGlobalVariables.clear();
  return false;
}

Pass *llvm::createGlobalMergePass(const TargetMachine *TM, unsigned Offset) {
  return new GlobalMerge(TM, Offset);
}
