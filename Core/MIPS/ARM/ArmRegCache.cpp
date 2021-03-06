// Copyright (C) 2003 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/

#include "ArmRegCache.h"
#include "ArmEmitter.h"

using namespace ArmGen;

#define CTXREG (R10)

ArmRegCache::ArmRegCache(MIPSState *mips) : mips_(mips) {
}

void ArmRegCache::Init(ARMXEmitter *emitter) {
	emit = emitter;
}

void ArmRegCache::Start(MIPSAnalyst::AnalysisResults &stats) {
	for (int i = 0; i < 16; i++) {
		ar[i].mipsReg = -1;
		ar[i].isDirty = false;
	}
	for (int i = 0; i < NUM_MIPSREG; i++) {
		mr[i].loc = ML_MEM;
		mr[i].reg = INVALID_REG;
		mr[i].imm = -1;
		mr[i].spillLock = false;
	}
}

static const ARMReg *GetMIPSAllocationOrder(int &count) {
	// Note that R0 and R1 are reserved as scratch for now. We can probably free up R1 eventually.
	// R8 is used to preserve flags in nasty branches.
	// R9 and upwards are reserved for jit basics.
	// Six allocated registers should be enough...
	static const ARMReg allocationOrder[] = {
		R2, R3, R4, R5, R6, R7
	};
	count = sizeof(allocationOrder) / sizeof(const int);
	return allocationOrder;
}

ARMReg ArmRegCache::MapReg(MIPSReg mipsReg, int mapFlags) {
	// Let's see if it's already mapped. If so we just need to update the dirty flag.
	// We don't need to check for ML_NOINIT because we assume that anyone who maps
	// with that flag immediately writes a "known" value to the register.
	if (mr[mipsReg].loc == ML_ARMREG) {
		if (ar[mr[mipsReg].reg].mipsReg != mipsReg) {
			ERROR_LOG(HLE, "Register mapping out of sync! %i", mipsReg);
		}
		if (mapFlags & MAP_DIRTY) {
			ar[mr[mipsReg].reg].isDirty = true;
		}
		return mr[mipsReg].reg;
	}

	// Okay, not mapped, so we need to allocate an ARM register.

	int allocCount;
	const ARMReg *allocOrder = GetMIPSAllocationOrder(allocCount);

allocate:
	for (int i = 0; i < allocCount; i++) {
		int reg = allocOrder[i];

		if (ar[reg].mipsReg == -1) {
			// That means it's free. Grab it, and load the value into it (if requested).
			ar[reg].isDirty = (mapFlags & MAP_DIRTY) ? true : false;
			if (!(mapFlags & MAP_NOINIT)) {
				if (mr[mipsReg].loc == ML_MEM) {
					if (mipsReg != 0) {
						emit->LDR((ARMReg)reg, CTXREG, 4 * mipsReg);
					} else {
						// If we get a request to load the zero register, at least we won't spend
						// time on a memory access...
						emit->MOV((ARMReg)reg, 0);
					}
				} else if (mr[mipsReg].loc == ML_IMM) {
					emit->ARMABI_MOVI2R((ARMReg)reg, mr[mipsReg].imm);
					ar[reg].isDirty = true;  // IMM is always dirty.
				}
			}
			ar[reg].mipsReg = mipsReg;
			mr[mipsReg].loc = ML_ARMREG;
			mr[mipsReg].reg = (ARMReg)reg;
			return (ARMReg)reg;
		}
	}

	// Still nothing. Let's spill a reg and goto 10.
	// TODO: Use age or something to choose which register to spill?
	// TODO: Spill dirty regs first? or opposite?
	int bestToSpill = -1;
	for (int i = 0; i < allocCount; i++) {
		int reg = allocOrder[i];
		if (ar[reg].mipsReg != -1 && mr[ar[reg].mipsReg].spillLock)
			continue;
		bestToSpill = reg;
		break;
	}

	if (bestToSpill != -1) {
		// ERROR_LOG(JIT, "Out of registers at PC %08x - spills register %i.", mips_->pc, bestToSpill);
		FlushArmReg((ARMReg)bestToSpill);
		goto allocate;
	}

	// Uh oh, we have all them spilllocked....
	ERROR_LOG(JIT, "Out of spillable registers at PC %08x!!!", mips_->pc);
	return INVALID_REG;
}

void ArmRegCache::MapInIn(MIPSReg rd, MIPSReg rs) {
	SpillLock(rd, rs);
	MapReg(rd);
	MapReg(rs);
	ReleaseSpillLocks();
}

void ArmRegCache::MapDirtyIn(MIPSReg rd, MIPSReg rs, bool avoidLoad) {
	SpillLock(rd, rs);
	bool overlap = avoidLoad && rd == rs;
	MapReg(rd, MAP_DIRTY | (overlap ? 0 : MAP_NOINIT));
	MapReg(rs);
	ReleaseSpillLocks();
}

void ArmRegCache::MapDirtyInIn(MIPSReg rd, MIPSReg rs, MIPSReg rt, bool avoidLoad) {
	SpillLock(rd, rs, rt);
	bool overlap = avoidLoad && (rd == rs || rd == rt);
	MapReg(rd, MAP_DIRTY | (overlap ? 0 : MAP_NOINIT));
	MapReg(rt);
	MapReg(rs);
	ReleaseSpillLocks();
}

void ArmRegCache::FlushArmReg(ARMReg r) {
	if (ar[r].mipsReg == -1) {
		// Nothing to do, reg not mapped.
		return;
	}
	if (ar[r].mipsReg != -1) {
		if (ar[r].isDirty && mr[ar[r].mipsReg].loc == ML_ARMREG)
			emit->STR(CTXREG, r, 4 * ar[r].mipsReg);
		// IMMs won't be in an ARM reg.
		mr[ar[r].mipsReg].loc = ML_MEM;
		mr[ar[r].mipsReg].reg = INVALID_REG;
		mr[ar[r].mipsReg].imm = 0;
	} else {
		ERROR_LOG(HLE, "Dirty but no mipsreg?");
	}
	ar[r].isDirty = false;
	ar[r].mipsReg = -1;
}

void ArmRegCache::FlushMipsReg(MIPSReg r) {
	switch (mr[r].loc) {
	case ML_IMM:
		// IMM is always "dirty".
		emit->ARMABI_MOVI2R(R0, mr[r].imm);
		emit->STR(CTXREG, R0, GetMipsRegOffset(r));
		break;

	case ML_ARMREG:
		if (mr[r].reg == INVALID_REG) {
			ERROR_LOG(HLE, "FlushMipsReg: MipsReg had bad ArmReg");
		}
		if (ar[mr[r].reg].isDirty) {
			emit->STR(CTXREG, mr[r].reg, GetMipsRegOffset(r));
			ar[mr[r].reg].isDirty = false;
		}
		ar[mr[r].reg].mipsReg = -1;
		break;

	case ML_MEM:
		// Already there, nothing to do.
		break;

	default:
		//BAD
		break;
	}
	mr[r].loc = ML_MEM;
	mr[r].reg = INVALID_REG;
	mr[r].imm = 0;
}

void ArmRegCache::FlushAll() {
	for (int i = 0; i < NUM_MIPSREG; i++) {
		FlushMipsReg(i);
	}
	// Sanity check
	for (int i = 0; i < NUM_ARMREG; i++) {
		if (ar[i].mipsReg != -1) {
			ERROR_LOG(JIT, "Flush fail: ar[%i].mipsReg=%i", i, ar[i].mipsReg);
		}
	}
}

void ArmRegCache::SetImm(MIPSReg r, u32 immVal) {
	// Zap existing value if cached in a reg
	if (mr[r].loc == ML_ARMREG) {
		ar[mr[r].reg].mipsReg = -1;
		ar[mr[r].reg].isDirty = false;
	}
	mr[r].loc = ML_IMM;
	mr[r].imm = immVal;
	mr[r].reg = INVALID_REG;
}

bool ArmRegCache::IsImm(MIPSReg r) const {
	return mr[r].loc == ML_IMM;
}

u32 ArmRegCache::GetImm(MIPSReg r) const {
	if (mr[r].loc != ML_IMM) {
		ERROR_LOG(JIT, "Trying to get imm from non-imm register %i", r);
	}
	return mr[r].imm;
}

int ArmRegCache::GetMipsRegOffset(MIPSReg r) {
	if (r < 32)
		return r * 4;
	switch (r) {
	case MIPSREG_HI:
		return offsetof(MIPSState, hi);
	case MIPSREG_LO:
		return offsetof(MIPSState, lo);
	}
	ERROR_LOG(JIT, "bad mips register %i", r);
	return 0;  // or what?
}

void ArmRegCache::SpillLock(MIPSReg r1, MIPSReg r2, MIPSReg r3) {
	mr[r1].spillLock = true;
	if (r2 != -1) mr[r2].spillLock = true;
	if (r3 != -1) mr[r3].spillLock = true;
}

void ArmRegCache::ReleaseSpillLocks() {
	for (int i = 0; i < NUM_MIPSREG; i++) {
		mr[i].spillLock = false;
	}
}

ARMReg ArmRegCache::R(int mipsReg) {
	if (mr[mipsReg].loc == ML_ARMREG) {
		return mr[mipsReg].reg;
	} else {
		ERROR_LOG(JIT, "Reg %i not in arm reg. compilerPC = %08x", mipsReg, compilerPC_);
		return INVALID_REG;  // BAAAD
	}
}
