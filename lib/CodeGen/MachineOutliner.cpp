//===---- MachineOutliner.cpp - Outline instructions -----------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implementation of MachineOutliner.h
///
//===----------------------------------------------------------------------===//

#include "MachineOutliner.h"

STATISTIC(NumOutlinedStat, "Number of candidates outlined");
STATISTIC(FunctionsCreatedStat, "Number of functions created");

using namespace llvm;

char MachineOutliner::ID = 0;

void MachineOutliner::buildProxyString(std::vector<unsigned> &Container,
                                       MachineBasicBlock *BB,
                                       const TargetRegisterInfo *TRI,
                                       const TargetInstrInfo *TII) {
  for (auto BBI = BB->instr_begin(), BBE = BB->instr_end(); BBI != BBE; BBI++) {

    // First, check if the current instruction is legal to outline at all.
    bool IsSafeToOutline = TII->isLegalToOutline(*BBI);

    // If it's not, give it a bad number.
    if (!IsSafeToOutline) {
      Container.push_back(CurrIllegalInstrMapping);
      CurrIllegalInstrMapping--;
    }

    // It's safe to outline, so we should give it a legal integer. If it's in
    // the map, then give it the previously assigned integer. Otherwise, give
    // it the next available one.
    else {
      auto Mapping = InstructionIntegerMap.find(&*BBI);

      if (Mapping != InstructionIntegerMap.end()) {
        Container.push_back(Mapping->second);
      }

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

void MachineOutliner::buildCandidateList(
    std::vector<Candidate> &CandidateList,
    std::vector<OutlinedFunction> &FunctionList,
    std::vector<MachineBasicBlock *> &Worklist) {

  // TODO: It would be better to use a "most beneficial substring" query if we
  // decide to be a bit smarter and use a dynamic programming approximation
  // scheme. For a naive greedy choice, LRS and MBS appear to be about as
  // effective as each other. This is because both can knock out a candidate
  // that would be better, or would lead to a better combination of candidates
  // being chosen.
  String *CandidateString = ST->longestRepeatedSubstring();

  assert(CandidateString && "no candidate");

  // FIXME: Use the following cost model.
  // Weight = Occurrences * length
  // Benefit = Weight - [Len(outline prologue) + Len(outline epilogue) +
  // Len(functon call)]
  //
  // TODO: Experiment with dynamic programming-based approximation scheme. If it
  // isn't too memory intensive, we really ought to switch to it.
  if (CandidateString != nullptr && CandidateString->size() >= 2) {
    size_t FunctionsCreated = 0;
    StringCollection SC = ST->InputString;
    std::vector<std::pair<String *, size_t>> *Occurrences =
        ST->findOccurrencesAndPrune(*CandidateString);

    // Query the tree for candidates until we run out of candidates to outline.
    do {
      assert(Occurrences != nullptr &&
             "Null occurrences for longestRepeatedSubstring!");

      // If there are at least two occurrences of this candidate, then we should
      // make it a function and keep track of it.
      if (Occurrences->size() >= 2) {
        std::pair<String *, size_t> FirstOcc = (*Occurrences)[0];

        // The index of the first character of the candidate in the 2D string.
        size_t IndexIn2DString = FirstOcc.second;

        // Use that to find the index of the string/MachineBasicBlock it appears
        // in and the point that it begins in in that string/MBB.
        std::pair<size_t, size_t> FirstIndexAndOffset =
            getStringIndexAndOffset(SC, IndexIn2DString);

        // From there, we can tell where the string starts and ends in the first
        // occurrence so that we can copy it over.
        size_t StartIdxInBB = FirstIndexAndOffset.second;
        size_t EndIdxInBB = StartIdxInBB + CandidateString->size() - 1;

        // Keep track of the MachineBasicBlock and its parent so that we can
        // copy from it later.
        MachineBasicBlock *OccBB = Worklist[FirstIndexAndOffset.first];

        FunctionList.push_back(
            OutlinedFunction(OccBB, StartIdxInBB, EndIdxInBB, FunctionsCreated,
                             CurrentFunctionID, Occurrences->size()));

        // Save each of the occurrences for the outlining process.
        for (auto &Occ : *Occurrences) {
          std::pair<size_t, size_t> IndexAndOffset =
              getStringIndexAndOffset(SC, Occ.second);
          CandidateList.push_back(Candidate(
              IndexAndOffset.first, // Idx of MBB containing candidate.
              IndexAndOffset.second,   // Starting idx in that MBB.
              CandidateString->size(), // Length of the candidate.
              Occ.second,              // Start index in the full string.
              FunctionsCreated, // Index of the corresponding OutlinedFunction.
              CandidateString // The actual string.
              )
          );
        }

        CurrentFunctionID++;
        FunctionsCreatedStat++;
      }

      // Find the next candidate and continue the process.
      CandidateString = ST->longestRepeatedSubstring();
    } while (CandidateString && CandidateString->size() >= 2 &&
             (Occurrences = ST->findOccurrencesAndPrune(*CandidateString)));

    // Sort the candidates in decending order. This will simplify the outlining
    // process when we have to remove the candidates from the string by
    // allowing us to cut them out without keeping track of an offset.
    std::sort(CandidateList.begin(), CandidateList.end());
  }
}

MachineFunction *
MachineOutliner::createOutlinedFunction(Module &M, const OutlinedFunction &OF) {

  // Create the function name and store it in the list of function names.
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

  auto StartIt = OF.OccBB->instr_begin();
  auto EndIt = StartIt;
  std::advance(StartIt, OF.StartIdxInBB);
  std::advance(EndIt, OF.EndIdxInBB);

  // Insert instructions into the function and a custom outlined
  // prologue/epilogue.
  MF.insert(MF.begin(), MBB);
  TII->insertOutlinerEpilog(MBB, MF);

  MachineInstr *MI;
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

bool MachineOutliner::outline(Module &M,
                              std::vector<MachineBasicBlock *> &Worklist,
                              std::vector<Candidate> &CandidateList,
                              std::vector<OutlinedFunction> &FunctionList) {
  StringCollection SC = ST->InputString;
  bool OutlinedSomething = false;

  // Create an outlined function for each candidate.
  for (size_t i = 0, e = FunctionList.size(); i < e; i++) {
    OutlinedFunction OF = FunctionList[i];
    FunctionList[i].MF = createOutlinedFunction(M, OF);
  }

  // Replace the candidates with calls to their respective outlined functions.
  //
  // FIXME: Change the suffix tree pruning technique so that it follows the
  // *longest* path on each internal node which *contains the node* that we're
  // invalidating stuff *for*. This will allow us to catch cases like this:
  // Outline "123", Outline "112". This method would make this unnecessary.
  //
  // FIXME: Currently, this method can allow us to unnecessarily outline stuff.
  // This should be done *before* we create the outlined functions.
  for (const Candidate &C : CandidateList) {

    size_t StartIndex = C.BBOffset;
    size_t EndIndex = StartIndex + C.Length;

    // If the index is below 0, then we must have already outlined from it.
    bool AlreadyOutlinedFrom = EndIndex - StartIndex > C.Length;

    // Check if we have any different characters in the string collection versus
    // the string we want to outline. If so, then we must have already outlined
    // from the spot this candidate appeared at.
    if (!AlreadyOutlinedFrom) {
      size_t j = 0;
      for (size_t i = StartIndex; i < EndIndex; i++) {
        if ((*SC[C.BBIndex])[i] != (*(C.Str))[j]) {
          FunctionList[C.FunctionIdx].OccurrenceCount--;
          AlreadyOutlinedFrom = true;
          break;
        }
        j++;
      }
    }

    // If we've outlined from this spot, or we don't have enough occurrences to
    // justify outlining stuff, then skip this candidate.
    if (AlreadyOutlinedFrom || FunctionList[C.FunctionIdx].OccurrenceCount < 2)
      continue;

    // We have a candidate which doesn't conflict with any other candidates, so
    // we can go ahead and outline it.
    OutlinedSomething = true;
    NumOutlinedStat++;

    // Remove the candidate from the string in the suffix tree first, and
    // replace it with the associated function's id.
    auto Begin = SC[C.BBIndex]->begin() + C.BBOffset;
    auto End = Begin + C.Length;

    SC[C.BBIndex]->erase(Begin, End);
    SC[C.BBIndex]->insert(Begin, FunctionList[C.FunctionIdx].Id);

    // Now outline the function in the module using the same idea.
    MachineFunction *MF = FunctionList[C.FunctionIdx].MF;
    MachineBasicBlock *MBB = Worklist[C.BBIndex];
    const TargetSubtargetInfo *target = &(MF->getSubtarget());
    const TargetInstrInfo *TII = target->getInstrInfo();
    MCContext &Ctx = MF->getContext();

    // We need the function name to match up with the internal symbol
    // build for it. There's no nice way to do this, so we'll just stick
    // an l_ in front of it manually.
    Twine InternalName = Twine("l_", MF->getName());
    MCSymbol *Name = Ctx.getOrCreateSymbol(InternalName);

    // Now, insert the function name and delete the instructions we don't need.
    auto It = MBB->instr_begin();
    auto StartIt = It;
    auto EndIt = It;

    std::advance(StartIt, StartIndex);
    std::advance(EndIt, EndIndex);
    StartIt = TII->insertOutlinedCall(MBB, StartIt, MF, Name);
    ++StartIt;
    MBB->erase(StartIt, EndIt);
  }

  return OutlinedSomething;
}

bool MachineOutliner::runOnModule(Module &M) {
  MachineModuleInfo &MMI = getAnalysis<MachineModuleInfo>();

  std::vector<MachineBasicBlock *> Worklist;

  // The current number we'll assign to instructions we ought not to outline.
  CurrIllegalInstrMapping = -1;

  // The current number we'll assign to instructions we want to outline.
  CurrLegalInstrMapping = 0;
  std::vector<std::vector<unsigned> *> Collection;

  // Set up the suffix tree by creating strings for each basic block.
  // Note: This means that the i-th string and the i-th MachineBasicBlock
  // in the work list correspond to each other. It also means that the
  // j-th character in that string and the j-th instruction in that
  // MBB correspond with each other.
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
      std::vector<unsigned> Container;
      buildProxyString(Container, MBB, TRI, TII);
      String *BBString = new String(Container);
      Collection.push_back(BBString);
    }
  }

  ST = new SuffixTree(Collection);

  // Find all of the candidates for outlining.
  bool OutlinedSomething = false;
  std::vector<Candidate> CandidateList;
  std::vector<OutlinedFunction> FunctionList;

  CurrentFunctionID = InstructionIntegerMap.size();
  buildCandidateList(CandidateList, FunctionList, Worklist);
  OutlinedSomething = outline(M, Worklist, CandidateList, FunctionList);

  delete ST;

  for (size_t i = 0, e = Collection.size(); i != e; i++)
    delete Collection[i];

  return OutlinedSomething;
}
