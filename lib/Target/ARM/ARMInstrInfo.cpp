//===-- ARMInstrInfo.cpp - ARM Instruction Information --------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains the ARM implementation of the TargetInstrInfo class.
//
//===----------------------------------------------------------------------===//

#include "ARMInstrInfo.h"
#include "ARM.h"
#include "ARMConstantPoolValue.h"
#include "ARMMachineFunctionInfo.h"
#include "ARMTargetMachine.h"
#include "MCTargetDesc/ARMAddressingModes.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/CodeGen/LiveVariables.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineJumpTableInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCInst.h"
using namespace llvm;

ARMInstrInfo::ARMInstrInfo(const ARMSubtarget &STI)
    : ARMBaseInstrInfo(STI), RI() {}

/// getNoopForMachoTarget - Return the noop instruction to use for a noop.
void ARMInstrInfo::getNoopForMachoTarget(MCInst &NopInst) const {
  if (hasNOP()) {
    NopInst.setOpcode(ARM::HINT);
    NopInst.addOperand(MCOperand::createImm(0));
    NopInst.addOperand(MCOperand::createImm(ARMCC::AL));
    NopInst.addOperand(MCOperand::createReg(0));
  } else {
    NopInst.setOpcode(ARM::MOVr);
    NopInst.addOperand(MCOperand::createReg(ARM::R0));
    NopInst.addOperand(MCOperand::createReg(ARM::R0));
    NopInst.addOperand(MCOperand::createImm(ARMCC::AL));
    NopInst.addOperand(MCOperand::createReg(0));
    NopInst.addOperand(MCOperand::createReg(0));
  }
}

unsigned ARMInstrInfo::getUnindexedOpcode(unsigned Opc) const {
  switch (Opc) {
  default:
    break;
  case ARM::LDR_PRE_IMM:
  case ARM::LDR_PRE_REG:
  case ARM::LDR_POST_IMM:
  case ARM::LDR_POST_REG:
    return ARM::LDRi12;
  case ARM::LDRH_PRE:
  case ARM::LDRH_POST:
    return ARM::LDRH;
  case ARM::LDRB_PRE_IMM:
  case ARM::LDRB_PRE_REG:
  case ARM::LDRB_POST_IMM:
  case ARM::LDRB_POST_REG:
    return ARM::LDRBi12;
  case ARM::LDRSH_PRE:
  case ARM::LDRSH_POST:
    return ARM::LDRSH;
  case ARM::LDRSB_PRE:
  case ARM::LDRSB_POST:
    return ARM::LDRSB;
  case ARM::STR_PRE_IMM:
  case ARM::STR_PRE_REG:
  case ARM::STR_POST_IMM:
  case ARM::STR_POST_REG:
    return ARM::STRi12;
  case ARM::STRH_PRE:
  case ARM::STRH_POST:
    return ARM::STRH;
  case ARM::STRB_PRE_IMM:
  case ARM::STRB_PRE_REG:
  case ARM::STRB_POST_IMM:
  case ARM::STRB_POST_REG:
    return ARM::STRBi12;
  }

  return 0;
}

void ARMInstrInfo::expandLoadStackGuard(MachineBasicBlock::iterator MI) const {
  MachineFunction &MF = *MI->getParent()->getParent();
  const ARMSubtarget &Subtarget = MF.getSubtarget<ARMSubtarget>();
  const TargetMachine &TM = MF.getTarget();

  if (!Subtarget.useMovt(MF)) {
    if (TM.isPositionIndependent())
      expandLoadStackGuardBase(MI, ARM::LDRLIT_ga_pcrel, ARM::LDRi12);
    else
      expandLoadStackGuardBase(MI, ARM::LDRLIT_ga_abs, ARM::LDRi12);
    return;
  }

  if (!TM.isPositionIndependent()) {
    expandLoadStackGuardBase(MI, ARM::MOVi32imm, ARM::LDRi12);
    return;
  }

  const GlobalValue *GV =
      cast<GlobalValue>((*MI->memoperands_begin())->getValue());

  if (!Subtarget.isGVIndirectSymbol(GV)) {
    expandLoadStackGuardBase(MI, ARM::MOV_ga_pcrel, ARM::LDRi12);
    return;
  }

  MachineBasicBlock &MBB = *MI->getParent();
  DebugLoc DL = MI->getDebugLoc();
  unsigned Reg = MI->getOperand(0).getReg();
  MachineInstrBuilder MIB;

  MIB = BuildMI(MBB, MI, DL, get(ARM::MOV_ga_pcrel_ldr), Reg)
            .addGlobalAddress(GV, 0, ARMII::MO_NONLAZY);
  auto Flags = MachineMemOperand::MOLoad |
               MachineMemOperand::MODereferenceable |
               MachineMemOperand::MOInvariant;
  MachineMemOperand *MMO = MBB.getParent()->getMachineMemOperand(
      MachinePointerInfo::getGOT(*MBB.getParent()), Flags, 4, 4);
  MIB.addMemOperand(MMO);
  MIB = BuildMI(MBB, MI, DL, get(ARM::LDRi12), Reg);
  MIB.addReg(Reg, RegState::Kill).addImm(0);
  MIB.setMemRefs(MI->memoperands_begin(), MI->memoperands_end());
  AddDefaultPred(MIB);
}

bool ARMInstrInfo::isLegalToOutline(const MachineInstr &MI) const {
  int Dummy;

  // Don't outline returns or basic block terminators
  if (MI.isReturn() || MI.isTerminator())
    return false;

  // Don't outline anything that modifies or reads from the stack pointer.
  else if (MI.modifiesRegister(ARM::SP, &RI) || MI.readsRegister(ARM::SP, &RI))
    return false;
  else if (MI.modifiesRegister(ARM::PC, &RI) || MI.readsRegister(ARM::PC, &RI))
    return false;
  else if (MI.getFlag(MachineInstr::MIFlag::FrameSetup) ||
           MI.getFlag(MachineInstr::MIFlag::FrameDestroy))
    return false;
  else if (MI.isCFIInstruction())
    return false;
  else if (isLoadFromStackSlot(MI, Dummy) || isStoreToStackSlot(MI, Dummy))
    return false;
  else if (isLoadFromStackSlotPostFE(MI, Dummy) ||
           isStoreToStackSlotPostFE(MI, Dummy))
    return false;
  else if (MI.isLabel())
    return false;

  for (auto It = MI.operands_begin(), Et = MI.operands_end(); It != Et; It++) {
    if ((*It).isCPI() || (*It).isJTI() || (*It).isCFIIndex() || (*It).isFI() ||
        (*It).isTargetIndex())
      return false;
  }

  switch (MI.getOpcode()) {
  case ARM::MOVi16_ga_pcrel:
  case ARM::t2MOVi16_ga_pcrel:
  case ARM::MOVTi16_ga_pcrel:
  case ARM::t2MOVTi16_ga_pcrel:
  case ARM::tPICADD:
  case ARM::PICADD:
  case ARM::PICSTR:
  case ARM::PICSTRB:
  case ARM::PICSTRH:
  case ARM::PICLDR:
  case ARM::PICLDRB:
  case ARM::PICLDRH:
  case ARM::PICLDRSB:
  case ARM::PICLDRSH:
  case ARM::CONSTPOOL_ENTRY:
  case ARM::t2TBB_JT:
  case ARM::t2TBH_JT:
  case ARM::t2Int_eh_sjlj_setjmp:
  case ARM::t2Int_eh_sjlj_setjmp_nofp:
  case ARM::tInt_eh_sjlj_setjmp:
    return false;
    break;
  default:
    break;
  }

  return true;
}

void ARMInstrInfo::insertOutlinerProlog(MachineBasicBlock *MBB,
                                        MachineFunction &MF) const {

  MachineInstr *SaveLR = BuildMI(MF, DebugLoc(), get(ARM::STMDB_UPD), ARM::SP)
                             .addReg(ARM::SP)
                             .addImm(14)
                             .addReg(0)
                             .addReg(ARM::R7)
                             .addReg(ARM::LR);

  MachineInstr *MovSP = BuildMI(MF, DebugLoc(), get(ARM::MOVr), ARM::R7)
                            .addReg(ARM::SP)
                            .addImm(14)
                            .addReg(0)
                            .addReg(0);

  MBB->insert(MBB->instr_begin(), MovSP);
  MBB->insert(MBB->instr_begin(), SaveLR);
}

void ARMInstrInfo::insertOutlinerEpilog(MachineBasicBlock *MBB,
                                        MachineFunction &MF) const {
  MachineInstr *RestoreSP = BuildMI(MF, DebugLoc(), get(ARM::MOVr), ARM::SP)
                                .addReg(ARM::R7)
                                .addImm(14)
                                .addReg(0)
                                .addReg(0);

  MachineInstr *PopSP = BuildMI(MF, DebugLoc(), get(ARM::LDMIA_UPD), ARM::SP)
                            .addReg(ARM::SP)
                            .addImm(14)
                            .addReg(0)
                            .addReg(ARM::R7)
                            .addReg(ARM::LR);

  MachineInstr *BranchBack =
      BuildMI(MF, DebugLoc(), get(ARM::MOVPCLR)).addImm(14).addReg(0);

  MBB->insert(MBB->instr_begin(), BranchBack);
  MBB->insert(MBB->instr_begin(), PopSP);
  MBB->insert(MBB->instr_begin(), RestoreSP);
}

/// TODO: Insert calls!
MachineBasicBlock::instr_iterator
ARMInstrInfo::insertOutlinedCall(MachineBasicBlock *MBB,
                                 MachineBasicBlock::instr_iterator &It,
                                 MachineFunction *MF, MCSymbol *Name) const {
  errs() << "ARM: Inserted ADD instead of call.\n";
  It = MBB->insert(It, BuildMI(*MF, DebugLoc(), get(ARM::ADDrr), ARM::R0)
                           .addReg(ARM::R0)
                           .addReg(ARM::R1)
                           .addImm(14)
                           .addReg(0)
                           .addReg(0));
  return It;
}
