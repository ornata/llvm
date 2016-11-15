//===---- MachineOutliner.h - Outline instructions -------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// The MachineOutliner is a code size reduction pass. It finds repeated
// sequences of instructions and pulls them out into their own functions.
// By pulling out such repeated sequences, we can reduce code size.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_CODEGEN_MACHINEOUTLINER_H
#define LLVM_LIB_CODEGEN_MACHINEOUTLINER_H

#define DEBUG_TYPE "machine-outliner"

#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/SuffixTree.h"
#include "llvm/ADT/Twine.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetRegisterInfo.h"
#include "llvm/Target/TargetSubtargetInfo.h"
#include <sstream>

typedef size_t CharacterType;
typedef std::vector<CharacterType> ContainerType;
typedef TerminatedString<ContainerType, CharacterType> String;
typedef TerminatedStringList<ContainerType, CharacterType> StringCollection;
typedef SuffixTree<ContainerType, CharacterType> STree;

/// Helper struct that stores the basic block, the function, the string, and
/// location of a Candidate.
struct Candidate {
  MachineBasicBlock *BB;     // BB containing this Candidate
  MachineFunction *ParentMF; // Function containing bb
  String *Str;               // The actual string to outline
  size_t Length;             // str->length()
  size_t StartIdxInBB;       // Start index in the string
  size_t EndIdxInBB;         // End index in the string
  size_t FunctionIdx; // Index of the candidate's function in the function list

  Candidate(MachineBasicBlock *bb_, MachineFunction *bb_BBParent_, String *Str_,
            const size_t &Length_, const size_t &StartIdxInBB_,
            const size_t &EndIdxInBB_, const size_t &fn_Id_)
      : BB(bb_), ParentMF(bb_BBParent_), Str(Str_), Length(Length_),
        StartIdxInBB(StartIdxInBB_), EndIdxInBB(EndIdxInBB_),
        FunctionIdx(fn_Id_) {}

  bool operator<(const Candidate &rhs) const {
    return StartIdxInBB < rhs.StartIdxInBB;
  }
};

/// Output for candidates for debugging purposes.
raw_ostream &operator<<(raw_ostream &os, const Candidate &c) {
  os << *(c.Str) << "\n";
  os << "StartIdxInBB: " << c.StartIdxInBB << "\n";
  os << "EndIdxInBB: " << c.EndIdxInBB << "\n";
  return os;
}

/// Helper struct that stores information about an actual outlined function.
struct OutlinedFunction {
  MachineFunction *MF;       // The actual outlined function
  MachineBasicBlock *OccBB;  // The FIRST occurrence of its string
  MachineFunction *BBParent; // The BBParent of OccBB
  size_t IdxInSC;            // The start index in the string.
  size_t StartIdxInBB;    // The start index in OccBB.
  size_t EndIdxInBB;      // The end index in OccBB.
  size_t Name;            // The name of this function in the program.
  size_t Id;              // The ID of this function in the proxy string.
  size_t OccurrenceCount; // Number of times this function appeared.

  OutlinedFunction(MachineBasicBlock *OccBB_, MachineFunction *BBParent_,
                   const size_t &IdxInSC_, const size_t &length_,
                   const size_t &StartIdxInBB_, const size_t &EndIdxInBB_,
                   const size_t &Name_, const size_t &Id_,
                   const size_t &OccurrenceCount_)
      : OccBB(OccBB_), BBParent(BBParent_), IdxInSC(IdxInSC_),
        StartIdxInBB(StartIdxInBB_), EndIdxInBB(EndIdxInBB_), Name(Name_),
        Id(Id_), OccurrenceCount(OccurrenceCount_) {}
};

namespace llvm {
struct MachineOutliner : public ModulePass {
  static char ID;

  DenseMap<MachineInstr *, int, MachineInstrExpressionTrait>
      InstructionIntegerMap;
  STree *ST = nullptr;

  // Target information
  size_t FunctionCallOverhead; // TODO
  int CurrIllegalInstrMapping;
  int CurrLegalInstrMapping;
  size_t CurrentFunctionID;
  std::vector<std::string *> *FunctionNames; // FIXME: Release function names.

  bool runOnModule(Module &M) override;
  StringRef getPassName() const override { return "Outliner"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<MachineModuleInfo>();
    AU.addPreserved<MachineModuleInfo>();
    AU.setPreservesAll();
    ModulePass::getAnalysisUsage(AU);
  }

  MachineOutliner() : ModulePass(ID) {
    ST = new STree();

    // FIXME: Release function names
    FunctionNames = new std::vector<std::string *>;
  }

  // General outlining functions.
  void buildProxyString(ContainerType &Container, MachineBasicBlock *BB,
                        const TargetRegisterInfo *TRI,
                        const TargetInstrInfo *TII);
  bool outline(Module &M, std::vector<MachineBasicBlock *> &Worklist,
               std::vector<Candidate> &CandidateList,
               std::vector<OutlinedFunction> &FunctionList);
  size_t removeOutsideSameBB(std::vector<std::pair<String *, size_t>> &occ,
                             const size_t &length, StringCollection &sc);
  MachineFunction *createOutlinedFunction(Module &M,
                                          const OutlinedFunction &OF);
  void buildCandidateList(std::vector<Candidate> &CandidateList,
                          std::vector<OutlinedFunction> &FunctionList,
                          std::vector<MachineBasicBlock *> &Worklist);
};

// FIXME: Free after printing
std::vector<std::string *> *OutlinerFunctionNames;
ModulePass *createOutlinerPass() {
  MachineOutliner *OL = new MachineOutliner();
  OutlinerFunctionNames = OL->FunctionNames;
  return OL;
}

} // LLVM namespace


#endif //LLVM_LIB_CODEGEN_MACHINEOUTLINER_H
