// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "../../MemMap.h"
#include "../MIPSAnalyst.h"
#include "../../Config.h"

#include "Jit.h"
#include "RegCache.h"


#define _RS ((op>>21) & 0x1F)
#define _RT ((op>>16) & 0x1F)
#define _RD ((op>>11) & 0x1F)
#define _FS ((op>>11) & 0x1F)
#define _FT ((op>>16) & 0x1F)
#define _FD ((op>>6 ) & 0x1F)
#define _POS	((op>>6 ) & 0x1F)
#define _SIZE ((op>>11 ) & 0x1F)

// All functions should have CONDITIONAL_DISABLE, so we can narrow things down to a file quickly.
// Currently known non working ones should have DISABLE.

// #define CONDITIONAL_DISABLE Comp_Generic(op); return;
#define CONDITIONAL_DISABLE ;
#define DISABLE Comp_Generic(op); return;

namespace MIPSComp
{
	void Jit::CompITypeMemRead(u32 op, u32 bits, void (XEmitter::*mov)(int, int, X64Reg, OpArg), void *safeFunc)
	{
		CONDITIONAL_DISABLE;
		int offset = (signed short)(op&0xFFFF);
		int rt = _RT;
		int rs = _RS;

		gpr.Lock(rt, rs);
		gpr.BindToRegister(rt, rt == rs, true);

		if (gpr.R(rs).IsImm())
		{
			void *data = Memory::GetPointer(gpr.R(rs).GetImmValue() + offset);
			if (data)
			{
#ifdef _M_IX86
				(this->*mov)(32, bits, gpr.RX(rt), M(data));
#else
				(this->*mov)(32, bits, gpr.RX(rt), MDisp(RBX, gpr.R(rs).GetImmValue() + offset));
#endif
			}
			else
				gpr.SetImmediate32(rt, 0);
		}
		else
		{
			// We may not even need to move into EAX as a temporary.
			X64Reg addr;
			if (gpr.R(rs).IsSimpleReg())
			{
				// TODO: Maybe just add a check if it's away, don't mind copying to EAX instead...
				if (rs != rt)
					gpr.BindToRegister(rs, true, false);
				addr = gpr.RX(rs);
			}
			else
			{
				MOV(32, R(EAX), gpr.R(rs));
				addr = EAX;
			}

			if (!g_Config.bFastMemory)
			{
				// Is it in physical ram?
				CMP(32, R(addr), Imm32(0x08000000));
				FixupBranch tooLow = J_CC(CC_L);
				CMP(32, R(addr), Imm32(0x0A000000));
				FixupBranch tooHigh = J_CC(CC_GE);

				const u8* safe = GetCodePtr();
#ifdef _M_IX86
				(this->*mov)(32, bits, gpr.RX(rt), MDisp(addr, (u32)Memory::base + offset));
#else
				(this->*mov)(32, bits, gpr.RX(rt), MComplex(RBX, addr, SCALE_1, offset));
#endif

				FixupBranch skip = J();
				SetJumpTarget(tooLow);
				SetJumpTarget(tooHigh);

				// Might also be the scratchpad.
				CMP(32, R(addr), Imm32(0x00010000));
				FixupBranch tooLow2 = J_CC(CC_L);
				CMP(32, R(addr), Imm32(0x00014000));
				J_CC(CC_L, safe);
				SetJumpTarget(tooLow2);

				LEA(32, EAX, MDisp(addr, offset));
				ABI_CallFunctionA(thunks.ProtectFunction(safeFunc, 1), R(EAX));
				(this->*mov)(32, bits, gpr.RX(rt), R(EAX));

				SetJumpTarget(skip);
			}
			else
			{
#ifdef _M_IX86
				// Need to modify it, too bad.
				if (addr != EAX)
					MOV(32, R(EAX), gpr.R(rs));
				AND(32, R(EAX), Imm32(Memory::MEMVIEW32_MASK));
				(this->*mov)(32, bits, gpr.RX(rt), MDisp(EAX, (u32)Memory::base + offset));
#else
				(this->*mov)(32, bits, gpr.RX(rt), MComplex(RBX, addr, SCALE_1, offset));
#endif
			}
		}

		gpr.UnlockAll();
	}

	void Jit::CompITypeMemWrite(u32 op, u32 bits, void *safeFunc)
	{
		CONDITIONAL_DISABLE;
		int offset = (signed short)(op&0xFFFF);
		int rt = _RT;
		int rs = _RS;

		gpr.Lock(rt, rs);
		gpr.BindToRegister(rt, true, false);

#ifdef _M_IX86
		// We use EDX so we can have DL for 8-bit ops.
		const bool needSwap = bits == 8 && !gpr.R(rt).IsSimpleReg(EDX) && !gpr.R(rt).IsSimpleReg(ECX);
		if (needSwap)
			gpr.FlushLockX(EDX);
#endif

		if (gpr.R(rs).IsImm())
		{
			void *data = Memory::GetPointer(gpr.R(rs).GetImmValue() + offset);
			if (data)
			{
#ifdef _M_IX86
				if (needSwap)
				{
					MOV(32, R(EDX), gpr.R(rt));
					MOV(bits, M(data), R(EDX));
				}
				else
					MOV(bits, M(data), gpr.R(rt));
#else
				MOV(bits, MDisp(RBX, gpr.R(rs).GetImmValue() + offset), gpr.R(rt));
#endif
			}
		}
		else
		{
			// We may not even need to move into EAX as a temporary.
			X64Reg addr;
			if (gpr.R(rs).IsSimpleReg())
			{
				// TODO: Maybe just add a check if it's away, don't mind copying to EAX instead...
				if (rs != rt)
					gpr.BindToRegister(rs, true, false);
				addr = gpr.RX(rs);
			}
			else
			{
				MOV(32, R(EAX), gpr.R(rs));
				addr = EAX;
			}

			if (!g_Config.bFastMemory)
			{
				// Is it in physical ram?
				CMP(32, R(addr), Imm32(0x08000000));
				FixupBranch tooLow = J_CC(CC_L);
				CMP(32, R(addr), Imm32(0x0A000000));
				FixupBranch tooHigh = J_CC(CC_GE);

				const u8* safe = GetCodePtr();
#ifdef _M_IX86
				if (needSwap)
				{
					MOV(32, R(EDX), gpr.R(rt));
					MOV(bits, MDisp(addr, (u32)Memory::base + offset), R(EDX));
				}
				else
					MOV(bits, MDisp(addr, (u32)Memory::base + offset), gpr.R(rt));
#else
				MOV(bits, MComplex(RBX, addr, SCALE_1, offset), gpr.R(rt));
#endif

				FixupBranch skip = J();
				SetJumpTarget(tooLow);
				SetJumpTarget(tooHigh);

				// Might also be the scratchpad.
				CMP(32, R(addr), Imm32(0x00010000));
				FixupBranch tooLow2 = J_CC(CC_L);
				CMP(32, R(addr), Imm32(0x00014000));
				J_CC(CC_L, safe);
				SetJumpTarget(tooLow2);

				LEA(32, EAX, MDisp(addr, offset));
				ABI_CallFunctionAA(thunks.ProtectFunction(safeFunc, 2), gpr.R(rt), R(EAX));

				SetJumpTarget(skip);
			}
			else
			{
#ifdef _M_IX86
				// Need to modify it, too bad.
				if (addr != EAX)
					MOV(32, R(EAX), gpr.R(rs));
				AND(32, R(EAX), Imm32(Memory::MEMVIEW32_MASK));
				if (needSwap)
				{
					MOV(32, R(EDX), gpr.R(rt));
					MOV(bits, MDisp(EAX, (u32)Memory::base + offset), R(EDX));
				}
				else
					MOV(bits, MDisp(EAX, (u32)Memory::base + offset), gpr.R(rt));
#else
				MOV(bits, MComplex(RBX, addr, SCALE_1, offset), gpr.R(rt));
#endif
			}
		}

#ifdef _M_IX86
		if (needSwap)
			gpr.UnlockAllX();
#endif
		gpr.UnlockAll();
	}

	void Jit::Comp_ITypeMem(u32 op)
	{
		CONDITIONAL_DISABLE;
		int offset = (signed short)(op&0xFFFF);
		int rt = _RT;
		int rs = _RS;
		int o = op>>26;
		if (((op >> 29) & 1) == 0 && rt == 0) {
			// Don't load anything into $zr
			return;
		}

		switch (o)
		{
		case 37: //R(rt) = ReadMem16(addr); break; //lhu
			CompITypeMemRead(op, 16, &XEmitter::MOVZX, (void *) &Memory::Read_U16);
			break;

		case 36: //R(rt) = ReadMem8 (addr); break; //lbu
			CompITypeMemRead(op, 8, &XEmitter::MOVZX, (void *) &Memory::Read_U8);
			break;

		case 35: //R(rt) = ReadMem32(addr); break; //lw
			CompITypeMemRead(op, 32, &XEmitter::MOVZX, (void *) &Memory::Read_U32);
			break;

		case 32: //R(rt) = (u32)(s32)(s8) ReadMem8 (addr); break; //lb
			CompITypeMemRead(op, 8, &XEmitter::MOVSX, (void *) &Memory::Read_U8);
			break;

		case 33: //R(rt) = (u32)(s32)(s16)ReadMem16(addr); break; //lh
			CompITypeMemRead(op, 16, &XEmitter::MOVSX, (void *) &Memory::Read_U16);
			break;

		case 40: //WriteMem8 (addr, R(rt)); break; //sb
			CompITypeMemWrite(op, 8, (void *) &Memory::Write_U8);
			break;

		case 41: //WriteMem16(addr, R(rt)); break; //sh
			CompITypeMemWrite(op, 16, (void *) &Memory::Write_U16);
			break;

		case 43: //WriteMem32(addr, R(rt)); break; //sw
			CompITypeMemWrite(op, 32, (void *) &Memory::Write_U32);
			break;

		case 134: //lwl
			{
				Crash();
				//u32 shift = (addr & 3) << 3;
				//u32 mem = ReadMem32(addr & 0xfffffffc);
				//R(rt) = ( u32(R(rt)) & (0x00ffffff >> shift) ) | ( mem << (24 - shift) );
			}
			break;

		case 138: //lwr
			{
				Crash();
				//u32 shift = (addr & 3) << 3;
				//u32 mem = ReadMem32(addr & 0xfffffffc);

				//R(rt) = ( u32(rt) & (0xffffff00 << (24 - shift)) ) | ( mem	>> shift );
			}
			break;

		case 142: //swl
			{
				Crash();
				//u32 shift = (addr & 3) << 3;
				//u32 mem = ReadMem32(addr & 0xfffffffc);
				//WriteMem32((addr & 0xfffffffc),	( ( u32(R(rt)) >>	(24 - shift) ) ) |
				//	(	mem & (0xffffff00 << shift) ));
			}
			break;
		case 146: //swr
			{
				Crash();
				//	u32 shift = (addr & 3) << 3;
			//	u32 mem = ReadMem32(addr & 0xfffffffc);
//
//				WriteMem32((addr & 0xfffffffc), ( ( u32(R(rt)) << shift ) |
//					(mem	& (0x00ffffff >> (24 - shift)) ) ) );
			}
			break;
		default:
			Comp_Generic(op);
			return ;
		}

	}
}
