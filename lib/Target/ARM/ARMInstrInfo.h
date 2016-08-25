//===-- ARMInstrInfo.h - ARM Instruction Information ------------*- C++ -*-===//
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

#ifndef LLVM_LIB_TARGET_ARM_ARMINSTRINFO_H
#define LLVM_LIB_TARGET_ARM_ARMINSTRINFO_H

#include "ARMBaseInstrInfo.h"
#include "ARMRegisterInfo.h"

namespace llvm {
class ARMSubtarget;

class ARMInstrInfo : public ARMBaseInstrInfo {
  ARMRegisterInfo RI;

public:
  explicit ARMInstrInfo(const ARMSubtarget &STI);

  /// getNoopForMachoTarget - Return the noop instruction to use for a noop.
  void getNoopForMachoTarget(MCInst &NopInst) const override;

  // Return the non-pre/post incrementing version of 'Opc'. Return 0
  // if there is not such an opcode.
  unsigned getUnindexedOpcode(unsigned Opc) const override;

  /// getRegisterInfo - TargetInstrInfo is a superset of MRegister info.  As
  /// such, whenever a client has an instance of instruction info, it should
  /// always be able to get register info as well (through this method).
  ///
  const ARMRegisterInfo &getRegisterInfo() const override { return RI; }

private:
  void expandLoadStackGuard(MachineBasicBlock::iterator MI) const override;

  //// Outliner stuff.
public:
  bool isLegalToOutline(const MachineInstr &MI) const override;
  void insertOutlinerEpilog(MachineBasicBlock *MBB,
                            MachineFunction &MF) const override;
  void insertOutlinerProlog(MachineBasicBlock *MBB,
                            MachineFunction &MF) const override;
  MachineBasicBlock::instr_iterator
  insertOutlinedCall(MachineBasicBlock *MBB,
                     MachineBasicBlock::instr_iterator &It, MachineFunction *MF,
                     MCSymbol *Name) const override;
};
}

#endif
