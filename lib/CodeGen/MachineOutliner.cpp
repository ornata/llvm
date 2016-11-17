//===---- MachineOutliner.cpp - Outline instructions -----------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Implementation of MachineOutliner.h
//
//===----------------------------------------------------------------------===//

#include "MachineOutliner.h"

STATISTIC(NumOutlinedStat, "Number of candidates outlined");
STATISTIC(FunctionsCreatedStat, "Number of functions created");

using namespace llvm;

char MachineOutliner::ID = 0;

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

    // If it is legal, we either insert it size_to the map, or get its existing
    // Id
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
  for (size_t i = 0, e = Occurrences.size(); i < e; i++) {
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
        FunctionsCreatedStat++;
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

  /// Insert the instructions from the candidate size_to the function, along
  /// with
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

  for (size_t i = 0, e = FunctionList.size(); i < e; i++) {
    OutlinedFunction OF = FunctionList[i];
    FunctionList[i].MF = createOutlinedFunction(M, OF);
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
    NumOutlinedStat++;

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
    size_t i;
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
