// Outliner - Transform repeated instruction sequences size_to function calls //
// ----------------------------------------------------------------------------
//
// This pass finds repeated, Identical sequences of instructions and replaces
// them with calls to a function.
// FIXME: This should go somewhere sane
// FIXME: There are some bugs where we get illegal register numbers sometimes
// or invalid instructions. This is probably to do with the MachineModulePass
// invalidating some MachineModuleInfo stuff, since the outliner doesn't
// change instructions.

#define DEBUG_TYPE "machine-outliner"

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

namespace {

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
  size_t Length;                // str->length()
  size_t StartIdxInBB;          // Start index in the string
  size_t EndIdxInBB;            // End index in the string
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
  bool Created;
  MachineFunction *MF;       // The actual outlined function
  MachineBasicBlock *OccBB;  // The FIRST occurrence of its string
  MachineFunction *BBParent; // The BBParent of OccBB
  size_t IdxInSC;               // The start index in the string.
  size_t length;                // The length of that string.
  size_t max_Idx;
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
        Id(Id_), OccurrenceCount(OccurrenceCount_) {
    Created = false;
  }
};

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
} // Anonymous namespace.

/// Construct a proxy string for a MachineBasicBlock.
void MachineOutliner::buildProxyString(ContainerType &Container,
                                       MachineBasicBlock *BB,
                                       const TargetRegisterInfo *TRI,
                                       const TargetInstrInfo *TII) {
  for (auto BBI = BB->instr_begin(), BBE = BB->instr_end(); BBI != BBE; BBI++) {

    // First, check if the current instruction is legal to outline at all
    bool IsSafeToOutline = TII->isLegalToOutline(*BBI);

    // If it's not, give it a bad number
    if (!IsSafeToOutline) {
      Container.push_back(CurrIllegalInstrMapping);
      CurrIllegalInstrMapping--;
    }

    // If it is legal, we either insert it size_to the map, or get its existing Id
    else {
      auto Mapping = InstructionIntegerMap.find(&*BBI);

      // It was found in the map...
      if (Mapping != InstructionIntegerMap.end()) {
        Container.push_back(Mapping->second);
      }

      // Otherwise, it wasn't there, so we should put it there!
      else {
        InstructionIntegerMap.insert(
            std::pair<MachineInstr *, int>(&*BBI, CurrLegalInstrMapping));
        Container.push_back(CurrLegalInstrMapping);
        CurrLegalInstrMapping++;
        CurrentFunctionID++;
      }
    }
  }
}

/// Remove candidates which don't lie within the same MachineBasicBlock.
size_t MachineOutliner::removeOutsideSameBB(
    std::vector<std::pair<String *, size_t>> &Occurrences, const size_t &Length,
    StringCollection &SC) {
  size_t Removed = 0;

  // StringLocation: first = index of the string, second = index size_to that
  // string.
  for (size_t i = 0; i < Occurrences.size(); i++) {
    auto StringLocation = SC.stringIndexContaining(Occurrences[i].second);
    if (StringLocation.second + Length - 1 >
        SC.stringAt(StringLocation.first).size()) {
      Occurrences.erase(Occurrences.begin() + i);
      Removed++;
    }
  }

  return Removed;
}

/// Find the potential outlining candidates for the program and return them in
/// CandidateList.
void MachineOutliner::buildCandidateList(
    std::vector<Candidate> &CandidateList,
    std::vector<OutlinedFunction> &FunctionList,
    std::vector<MachineBasicBlock *> &Worklist) {

  String *CandidateString = ST->longestRepeatedSubstring();

  // FIXME: That 2 should be a target-dependent minimum length.
  if (CandidateString != nullptr && CandidateString->length() >= 2) {
    size_t FunctionsCreated = 0;
    StringCollection SC = ST->SC;
    std::vector<std::pair<String *, size_t>> *Occurrences =
        ST->findOccurrences(*CandidateString);

    // Query the tree for candidates until we run out of candidates to outline.
    do {
      assert(Occurrences != nullptr &&
             "Null occurrences for longestRepeatedSubstring!");
      removeOutsideSameBB(*Occurrences, CandidateString->length(), SC);

      // If there are at least two occurrences of this candidate, then we should
      // make it a function and keep track of it.
      if (Occurrences->size() >= 2) {
        auto FirstOcc = (*Occurrences)[0];
        size_t IdxInSC = FirstOcc.second;
        auto StringLocation = ST->SC.stringIndexContaining(IdxInSC);
        size_t StartIdxInBB = StringLocation.second;
        size_t EndIdxInBB = StartIdxInBB + CandidateString->length() - 1;
        MachineBasicBlock *OccBB = Worklist[StringLocation.first];
        MachineFunction *BBParent = OccBB->getParent();

        FunctionList.push_back(OutlinedFunction(
            OccBB, BBParent, IdxInSC, CandidateString->length() - 1,
            StartIdxInBB, EndIdxInBB, FunctionsCreated, CurrentFunctionID,
            Occurrences->size()));

        // Save each of the occurrences for the outlining process.
        for (auto &Occ : *Occurrences)
          CandidateList.push_back(Candidate(
              OccBB, BBParent, CandidateString, CandidateString->length(),
              Occ.second, Occ.second + CandidateString->length(),
              FunctionsCreated));

        CurrentFunctionID++;
        FunctionsCreated++;
      }

      // Find the next candidate and continue the process.
      CandidateString = ST->longestRepeatedSubstring();
    } while (CandidateString && CandidateString->length() >= 2 &&
             (Occurrences = ST->findOccurrences(*CandidateString)));

    std::sort(CandidateList.begin(), CandidateList.end());

    DEBUG(for (size_t i = 0, e = CandidateList.size(); i < e; i++) {
      dbgs() << "Candidate " << i << ": \n";
      dbgs() << CandidateList[i] << "\n";
    });
  }
}

/// Create a new Function and MachineFunction for the OutlinedFunction OF. Place
/// that function in M.
MachineFunction *
MachineOutliner::createOutlinedFunction(Module &M, const OutlinedFunction &OF) {

  // Create the function name and store it size_to the function list.
  std::ostringstream NameStream;
  NameStream << "OUTLINED_FUNCTION" << OF.Name;
  std::string *Name = new std::string(NameStream.str());
  FunctionNames->push_back(Name);

  // Create the function using an IR-level function.
  LLVMContext &C = M.getContext();
  Function *F = dyn_cast<Function>(
      M.getOrInsertFunction(Name->c_str(), Type::getVoidTy(C), NULL));
  assert(F != nullptr);
  F->setLinkage(GlobalValue::PrivateLinkage);

  BasicBlock *EntryBB = BasicBlock::Create(C, "entry", F);
  IRBuilder<> Builder(EntryBB);
  Builder.CreateRetVoid();

  MachineModuleInfo &MMI = getAnalysis<MachineModuleInfo>();
  MachineFunction &MF = MMI.getMachineFunction(*F);
  MachineBasicBlock *MBB = MF.CreateMachineBasicBlock();
  const TargetSubtargetInfo *target = &(MF.getSubtarget());
  const TargetInstrInfo *TII = target->getInstrInfo();

  /// Find where the occurrence we want to copy starts and ends.
  DEBUG(dbgs() << "OF.StartIdxInBB = " << OF.StartIdxInBB << "\n";
        dbgs() << "OF.EndIdxInBB = " << OF.EndIdxInBB << "\n";);

  size_t i;
  auto StartIt = OF.OccBB->instr_begin();

  for (i = 0; i < OF.StartIdxInBB; i++)
    ++StartIt;

  auto EndIt = StartIt;

  for (; i < OF.EndIdxInBB; ++i)
    ++EndIt;

  /// Insert the instructions from the candidate size_to the function, along with
  /// the special epilogue and prologue for the outliner.
  MF.insert(MF.begin(), MBB);
  TII->insertOutlinerEpilog(MBB, MF);

  MachineInstr *MI;

  // Clone each machine instruction in the outlined range and insert them before
  // the inserted epilogue.
  while (EndIt != StartIt) {
    MI = MF.CloneMachineInstr(&*EndIt);
    MI->dropMemRefs();
    MBB->insert(MBB->instr_begin(), MI);
    EndIt--;
  }

  MI = MF.CloneMachineInstr(&*EndIt);
  MI->dropMemRefs();
  MBB->insert(MBB->instr_begin(), MI);

  TII->insertOutlinerProlog(MBB, MF);

  DEBUG(dbgs() << "New function: \n"; dbgs() << *Name << ":\n";
        for (auto MBB = MF.begin(), EBB = MF.end(); MBB != EBB;
             MBB++) { (&(*MBB))->dump(); });

  return &MF;
}

/// Find outlining candidates, create functions from them, and replace them with
/// function calls.
bool MachineOutliner::outline(Module &M,
                              std::vector<MachineBasicBlock *> &Worklist,
                              std::vector<Candidate> &CandidateList,
                              std::vector<OutlinedFunction> &FunctionList) {
  StringCollection SC = ST->SC;
  bool OutlinedSomething = false;
  int Offset = 0;

  for (size_t i = 0; i < FunctionList.size(); i++) {
    OutlinedFunction OF = FunctionList[i];
    FunctionList[i].MF = createOutlinedFunction(M, OF);
    FunctionList[i].Created = true;
  }

  /// Replace the candidates with calls to their respective outlined functions.
  for (const Candidate &C : CandidateList) {
    size_t OffsetedStringStart = C.StartIdxInBB + Offset;
    size_t OffsetedStringEnd = OffsetedStringStart + C.Length;

    /// If this spot doesn't match with our string, we must have already
    /// outlined something from here. Therefore, we should skip it to avoid
    /// overlaps.

    // If the offsetted string starts below index 0, we must have overlapped
    // something
    bool AlreadyOutlinedFrom = (OffsetedStringStart > OffsetedStringEnd);

    if (!AlreadyOutlinedFrom) {
      size_t j = 0;
      for (size_t i = OffsetedStringStart; i < OffsetedStringEnd; i++) {
        if (SC[i] != (*(C.Str))[j]) {
          FunctionList[C.FunctionIdx].OccurrenceCount--;
          AlreadyOutlinedFrom = true;
          break;
        }
        j++;
      }
    }

    if (AlreadyOutlinedFrom || FunctionList[C.FunctionIdx].OccurrenceCount < 2)
      continue;

    /// We have a candidate which doesn't conflict with any other candidates, so
    /// we can go ahead and outline it.
    OutlinedSomething = true;
    auto StringLocation = SC.stringIndexContaining(OffsetedStringStart);

    /// Update the proxy string.
    SC.insertBefore(OffsetedStringStart, FunctionList[C.FunctionIdx].Id);

    SC.erase(OffsetedStringStart + 1, OffsetedStringEnd + 1);

    /// Update the module.
    MachineFunction *MF = FunctionList[C.FunctionIdx].MF;
    MachineBasicBlock *MBB = Worklist[StringLocation.first];
    const TargetSubtargetInfo *target = &(MF->getSubtarget());
    const TargetInstrInfo *TII = target->getInstrInfo();

    /// Get the name of the function we want to insert a call to.
    MCContext &Ctx = MF->getContext();
    Twine size_ternalName = Twine("l_", MF->getName());
    MCSymbol *Name = Ctx.getOrCreateSymbol(size_ternalName);

    /// Find the start of the candidate's range, insert the call before it, and
    /// then delete the range.
    unsigned i;
    auto It = MBB->instr_begin();
    auto StartIt = It;
    auto EndIt = It;

    for (i = 0; i < StringLocation.second; ++i) {
      ++StartIt;
      ++It;
    }

    StartIt = TII->insertOutlinedCall(MBB, StartIt, MF, Name);
    ++Offset; // Inserted one character => everything shifts right by 1.
    ++StartIt;

    for (; i < StringLocation.second + C.Length; ++i)
      ++It;

    EndIt = It;

    MBB->erase(StartIt, EndIt);
    Offset -= C.Length;
  }

  return OutlinedSomething;
}

/// Construct the suffix tree for the program and run the outlining algorithm.
bool MachineOutliner::runOnModule(Module &M) {
  MachineModuleInfo &MMI = getAnalysis<MachineModuleInfo>();
  std::vector<MachineBasicBlock *> Worklist;

  CurrIllegalInstrMapping = -1;
  CurrLegalInstrMapping = 0;

  // Set up the suffix tree.
  for (auto MI = M.begin(), ME = M.end(); MI != ME; MI++) {
    Function *F = &*MI;
    MachineFunction &MF = MMI.getMachineFunction(*F);
    const TargetSubtargetInfo *target = &(MF.getSubtarget());
    const TargetRegisterInfo *TRI = target->getRegisterInfo();
    const TargetInstrInfo *TII = target->getInstrInfo();

    if (F->empty() || !TII->functionIsSafeToOutlineFrom(*F))
      continue;

    for (auto MFI = MF.begin(), MFE = MF.end(); MFI != MFE; ++MFI) {
      MachineBasicBlock *MBB = &*MFI;
      Worklist.push_back(MBB);
      ContainerType Container;
      buildProxyString(Container, MBB, TRI, TII);
      String *BBString = new String(Container);
      ST->append(BBString);
    }
  }
  // Find all of the candidates for outlining.
  bool OutlinedSomething = false;
  std::vector<Candidate> CandidateList;
  std::vector<OutlinedFunction> FunctionList;

  CurrentFunctionID = InstructionIntegerMap.size();
  buildCandidateList(CandidateList, FunctionList, Worklist);
  OutlinedSomething = outline(M, Worklist, CandidateList, FunctionList);

  delete ST;
  return OutlinedSomething;
}

char MachineOutliner::ID = 0;

// FIXME: Free after prsize_ting
std::vector<std::string *> *OutlinerFunctionNames;

ModulePass *createOutlinerPass() {
  MachineOutliner *OL = new MachineOutliner();
  OutlinerFunctionNames = OL->FunctionNames;
  return OL;
}
