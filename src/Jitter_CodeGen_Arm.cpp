#include "Jitter_CodeGen_Arm.h"
#include "ObjectFile.h"

using namespace Jitter;

CArmAssembler::REGISTER CCodeGen_Arm::g_baseRegister = CArmAssembler::r11;
CArmAssembler::REGISTER CCodeGen_Arm::g_callAddressRegister = CArmAssembler::r4;

CArmAssembler::REGISTER CCodeGen_Arm::g_registers[MAX_REGISTERS] =
{
	CArmAssembler::r4,
	CArmAssembler::r5,
	CArmAssembler::r6,
	CArmAssembler::r7,
	CArmAssembler::r8,
	CArmAssembler::r10,
};

CArmAssembler::REGISTER CCodeGen_Arm::g_paramRegs[MAX_PARAMS] =
{
	CArmAssembler::r0,
	CArmAssembler::r1,
	CArmAssembler::r2,
	CArmAssembler::r3,
};

extern "C" uint32 CodeGen_Arm_div_unsigned(uint32 a, uint32 b)
{
	return a / b;
}

extern "C" int32 CodeGen_Arm_div_signed(int32 a, int32 b)
{
	return a / b;
}

extern "C" uint32 CodeGen_Arm_mod_unsigned(uint32 a, uint32 b)
{
	return a % b;
}

extern "C" int32 CodeGen_Arm_mod_signed(int32 a, int32 b)
{
	return a % b;
}

template <typename ALUOP>
void CCodeGen_Arm::Emit_Alu_GenericAnyAny(const STATEMENT& statement)
{
	auto dst = statement.dst->GetSymbol().get();
	auto src1 = statement.src1->GetSymbol().get();
	auto src2 = statement.src2->GetSymbol().get();

	auto dstReg = PrepareSymbolRegisterDef(dst, CArmAssembler::r0);
	auto src1Reg = PrepareSymbolRegisterUse(src1, CArmAssembler::r1);
	auto src2Reg = PrepareSymbolRegisterUse(src2, CArmAssembler::r2);
	((m_assembler).*(ALUOP::OpReg()))(dstReg, src1Reg, src2Reg);
	CommitSymbolRegister(dst, dstReg);
}

template <typename ALUOP>
void CCodeGen_Arm::Emit_Alu_GenericAnyCst(const STATEMENT& statement)
{
	auto dst = statement.dst->GetSymbol().get();
	auto src1 = statement.src1->GetSymbol().get();
	auto src2 = statement.src2->GetSymbol().get();

	assert(src2->m_type == SYM_CONSTANT);

	auto dstReg = PrepareSymbolRegisterDef(dst, CArmAssembler::r0);
	auto src1Reg = PrepareSymbolRegisterUse(src1, CArmAssembler::r1);
	uint32 cst = src2->m_valueLow;

	bool supportsNegative	= ALUOP::OpImmNeg() != NULL;
	bool supportsComplement = ALUOP::OpImmNot() != NULL;
	
	uint8 immediate = 0;
	uint8 shiftAmount = 0;
	if(TryGetAluImmediateParams(cst, immediate, shiftAmount))
	{
		((m_assembler).*(ALUOP::OpImm()))(dstReg, src1Reg, CArmAssembler::MakeImmediateAluOperand(immediate, shiftAmount));
	}
	else if(supportsNegative && TryGetAluImmediateParams(-static_cast<int32>(cst), immediate, shiftAmount))
	{
		((m_assembler).*(ALUOP::OpImmNeg()))(dstReg, src1Reg, CArmAssembler::MakeImmediateAluOperand(immediate, shiftAmount));
	}
	else if(supportsComplement && TryGetAluImmediateParams(~cst, immediate, shiftAmount))
	{
		((m_assembler).*(ALUOP::OpImmNot()))(dstReg, src1Reg, CArmAssembler::MakeImmediateAluOperand(immediate, shiftAmount));
	}
	else
	{
		auto cstReg = PrepareSymbolRegisterUse(src2, CArmAssembler::r2);
		assert(cstReg != dstReg && cstReg != src1Reg);
		((m_assembler).*(ALUOP::OpReg()))(dstReg, src1Reg, cstReg);
	}

	CommitSymbolRegister(dst, dstReg);
}

#define ALU_CONST_MATCHERS(ALUOP_CST, ALUOP) \
	{ ALUOP_CST,	MATCH_ANY,		MATCH_ANY,		MATCH_CONSTANT,	&CCodeGen_Arm::Emit_Alu_GenericAnyCst<ALUOP>		}, \
	{ ALUOP_CST,	MATCH_ANY,		MATCH_ANY,		MATCH_ANY,		&CCodeGen_Arm::Emit_Alu_GenericAnyAny<ALUOP>		},

#include "Jitter_CodeGen_Arm_Div.h"

template <bool isSigned>
void CCodeGen_Arm::Emit_MulTmp64AnyAny(const STATEMENT& statement)
{
	auto dst = statement.dst->GetSymbol().get();
	auto src1 = statement.src1->GetSymbol().get();
	auto src2 = statement.src2->GetSymbol().get();

	auto resLoReg = CArmAssembler::r0;
	auto resHiReg = CArmAssembler::r1;
	auto src1Reg = PrepareSymbolRegisterUse(src1, CArmAssembler::r2);
	auto src2Reg = PrepareSymbolRegisterUse(src2, CArmAssembler::r3);
	
	assert(dst->m_type == SYM_TEMPORARY64);
	assert(resLoReg != src1Reg && resLoReg != src2Reg);
	assert(resHiReg != src1Reg && resHiReg != src2Reg);
	
	if(isSigned)
	{
		m_assembler.Smull(resLoReg, resHiReg, src1Reg, src2Reg);
	}
	else
	{
		m_assembler.Umull(resLoReg, resHiReg, src1Reg, src2Reg);
	}
	
	m_assembler.Str(resLoReg, CArmAssembler::rSP, CArmAssembler::MakeImmediateLdrAddress(dst->m_stackLocation + m_stackLevel + 0));
	m_assembler.Str(resHiReg, CArmAssembler::rSP, CArmAssembler::MakeImmediateLdrAddress(dst->m_stackLocation + m_stackLevel + 4));
}

template <CArmAssembler::SHIFT shiftType>
void CCodeGen_Arm::Emit_Shift_Generic(const STATEMENT& statement)
{
	CSymbol* dst = statement.dst->GetSymbol().get();
	CSymbol* src1 = statement.src1->GetSymbol().get();
	CSymbol* src2 = statement.src2->GetSymbol().get();

	auto dstReg = PrepareSymbolRegisterDef(dst, CArmAssembler::r0);
	auto src1Reg = PrepareSymbolRegisterUse(src1, CArmAssembler::r1);
	auto shift = GetAluShiftFromSymbol(shiftType, src2, CArmAssembler::r2);
	m_assembler.Mov(dstReg, CArmAssembler::MakeRegisterAluOperand(src1Reg, shift));
	CommitSymbolRegister(dst, dstReg);
}

CCodeGen_Arm::CONSTMATCHER CCodeGen_Arm::g_constMatchers[] = 
{ 
	{ OP_LABEL,			MATCH_NIL,			MATCH_NIL,			MATCH_NIL,			&CCodeGen_Arm::MarkLabel									},

	{ OP_NOP,			MATCH_NIL,			MATCH_NIL,			MATCH_NIL,			&CCodeGen_Arm::Emit_Nop										},
	
	{ OP_MOV,			MATCH_REGISTER,		MATCH_REGISTER,		MATCH_NIL,			&CCodeGen_Arm::Emit_Mov_RegReg								},
	{ OP_MOV,			MATCH_REGISTER,		MATCH_MEMORY,		MATCH_NIL,			&CCodeGen_Arm::Emit_Mov_RegMem								},
	{ OP_MOV,			MATCH_REGISTER,		MATCH_CONSTANT,		MATCH_NIL,			&CCodeGen_Arm::Emit_Mov_RegCst								},
	{ OP_MOV,			MATCH_MEMORY,		MATCH_REGISTER,		MATCH_NIL,			&CCodeGen_Arm::Emit_Mov_MemReg								},
	{ OP_MOV,			MATCH_MEMORY,		MATCH_MEMORY,		MATCH_NIL,			&CCodeGen_Arm::Emit_Mov_MemMem								},
	{ OP_MOV,			MATCH_MEMORY,		MATCH_CONSTANT,		MATCH_NIL,			&CCodeGen_Arm::Emit_Mov_MemCst								},

	ALU_CONST_MATCHERS(OP_ADD, ALUOP_ADD)
	ALU_CONST_MATCHERS(OP_SUB, ALUOP_SUB)
	ALU_CONST_MATCHERS(OP_AND, ALUOP_AND)
	ALU_CONST_MATCHERS(OP_OR,  ALUOP_OR)
	ALU_CONST_MATCHERS(OP_XOR, ALUOP_XOR)
	
	{ OP_SRL,			MATCH_ANY,			MATCH_ANY,			MATCH_ANY,			&CCodeGen_Arm::Emit_Shift_Generic<CArmAssembler::SHIFT_LSR>	},
	{ OP_SRA,			MATCH_ANY,			MATCH_ANY,			MATCH_ANY,			&CCodeGen_Arm::Emit_Shift_Generic<CArmAssembler::SHIFT_ASR>	},
	{ OP_SLL,			MATCH_ANY,			MATCH_ANY,			MATCH_ANY,			&CCodeGen_Arm::Emit_Shift_Generic<CArmAssembler::SHIFT_LSL>	},

	{ OP_PARAM,			MATCH_NIL,			MATCH_CONTEXT,		MATCH_NIL,			&CCodeGen_Arm::Emit_Param_Ctx								},
	{ OP_PARAM,			MATCH_NIL,			MATCH_REGISTER,		MATCH_NIL,			&CCodeGen_Arm::Emit_Param_Reg								},
	{ OP_PARAM,			MATCH_NIL,			MATCH_MEMORY,		MATCH_NIL,			&CCodeGen_Arm::Emit_Param_Mem								},
	{ OP_PARAM,			MATCH_NIL,			MATCH_CONSTANT,		MATCH_NIL,			&CCodeGen_Arm::Emit_Param_Cst								},
	{ OP_PARAM,			MATCH_NIL,			MATCH_MEMORY64,		MATCH_NIL,			&CCodeGen_Arm::Emit_Param_Mem64								},
	{ OP_PARAM,			MATCH_NIL,			MATCH_CONSTANT64,	MATCH_NIL,			&CCodeGen_Arm::Emit_Param_Cst64								},
	{ OP_PARAM,			MATCH_NIL,			MATCH_MEMORY128,	MATCH_NIL,			&CCodeGen_Arm::Emit_Param_Mem128							},

	{ OP_PARAM_RET,		MATCH_NIL,			MATCH_TEMPORARY128,	MATCH_NIL,			&CCodeGen_Arm::Emit_ParamRet_Tmp128							},

	{ OP_CALL,			MATCH_NIL,			MATCH_CONSTANT,		MATCH_CONSTANT,		&CCodeGen_Arm::Emit_Call									},
	
	{ OP_RETVAL,		MATCH_REGISTER,		MATCH_NIL,			MATCH_NIL,			&CCodeGen_Arm::Emit_RetVal_Reg								},
	{ OP_RETVAL,		MATCH_TEMPORARY,	MATCH_NIL,			MATCH_NIL,			&CCodeGen_Arm::Emit_RetVal_Tmp								},
	{ OP_RETVAL,		MATCH_MEMORY64,		MATCH_NIL,			MATCH_NIL,			&CCodeGen_Arm::Emit_RetVal_Mem64							},

	{ OP_JMP,			MATCH_NIL,			MATCH_NIL,			MATCH_NIL,			&CCodeGen_Arm::Emit_Jmp										},

	{ OP_CONDJMP,		MATCH_NIL,			MATCH_VARIABLE,		MATCH_CONSTANT,		&CCodeGen_Arm::Emit_CondJmp_VarCst							},
	{ OP_CONDJMP,		MATCH_NIL,			MATCH_VARIABLE,		MATCH_VARIABLE,		&CCodeGen_Arm::Emit_CondJmp_VarVar							},
	
	{ OP_CMP,			MATCH_ANY,			MATCH_ANY,			MATCH_CONSTANT,		&CCodeGen_Arm::Emit_Cmp_AnyAnyCst							},
	{ OP_CMP,			MATCH_ANY,			MATCH_ANY,			MATCH_ANY,			&CCodeGen_Arm::Emit_Cmp_AnyAnyAny							},

	{ OP_NOT,			MATCH_REGISTER,		MATCH_REGISTER,		MATCH_NIL,			&CCodeGen_Arm::Emit_Not_RegReg								},
	{ OP_NOT,			MATCH_MEMORY,		MATCH_REGISTER,		MATCH_NIL,			&CCodeGen_Arm::Emit_Not_MemReg								},
	{ OP_NOT,			MATCH_MEMORY,		MATCH_MEMORY,		MATCH_NIL,			&CCodeGen_Arm::Emit_Not_MemMem								},
	
	{ OP_DIV,			MATCH_TEMPORARY64,	MATCH_REGISTER,		MATCH_REGISTER,		&CCodeGen_Arm::Emit_DivTmp64RegReg<false>					},
	{ OP_DIV,			MATCH_TEMPORARY64,	MATCH_REGISTER,		MATCH_CONSTANT,		&CCodeGen_Arm::Emit_DivTmp64RegCst<false>					},
	{ OP_DIV,			MATCH_TEMPORARY64,	MATCH_MEMORY,		MATCH_CONSTANT,		&CCodeGen_Arm::Emit_DivTmp64MemCst<false>					},
//	{ OP_DIV,			MATCH_TEMPORARY64,	MATCH_RELATIVE,		MATCH_REGISTER,		&CCodeGen_Arm::Emit_DivTmp64RelReg<false>					},

	{ OP_DIVS,			MATCH_TEMPORARY64,	MATCH_REGISTER,		MATCH_REGISTER,		&CCodeGen_Arm::Emit_DivTmp64RegReg<true>					},
	{ OP_DIVS,			MATCH_TEMPORARY64,	MATCH_REGISTER,		MATCH_CONSTANT,		&CCodeGen_Arm::Emit_DivTmp64RegCst<true>					},
	{ OP_DIVS,			MATCH_TEMPORARY64,	MATCH_MEMORY,		MATCH_CONSTANT,		&CCodeGen_Arm::Emit_DivTmp64MemCst<true>					},
//	{ OP_DIVS,			MATCH_TEMPORARY64,	MATCH_RELATIVE,		MATCH_REGISTER,		&CCodeGen_Arm::Emit_DivTmp64RelReg<true>					},
	
	{ OP_MUL,			MATCH_TEMPORARY64,	MATCH_ANY,			MATCH_ANY,			&CCodeGen_Arm::Emit_MulTmp64AnyAny<false>					},
	{ OP_MULS,			MATCH_TEMPORARY64,	MATCH_ANY,			MATCH_ANY,			&CCodeGen_Arm::Emit_MulTmp64AnyAny<true>					},

	{ OP_ADDREF,		MATCH_TMP_REF,		MATCH_REL_REF,		MATCH_REGISTER,		&CCodeGen_Arm::Emit_AddRef_TmpRelReg						},
	{ OP_ADDREF,		MATCH_TMP_REF,		MATCH_REL_REF,		MATCH_CONSTANT,		&CCodeGen_Arm::Emit_AddRef_TmpRelCst						},
	
	{ OP_LOADFROMREF,	MATCH_REGISTER,		MATCH_TMP_REF,		MATCH_NIL,			&CCodeGen_Arm::Emit_LoadFromRef_RegTmp						},

	{ OP_STOREATREF,	MATCH_NIL,			MATCH_TMP_REF,		MATCH_REGISTER,		&CCodeGen_Arm::Emit_StoreAtRef_TmpReg						},
	{ OP_STOREATREF,	MATCH_NIL,			MATCH_TMP_REF,		MATCH_RELATIVE,		&CCodeGen_Arm::Emit_StoreAtRef_TmpRel						},
	{ OP_STOREATREF,	MATCH_NIL,			MATCH_TMP_REF,		MATCH_CONSTANT,		&CCodeGen_Arm::Emit_StoreAtRef_TmpCst						},
	
	{ OP_MOV,			MATCH_NIL,			MATCH_NIL,			MATCH_NIL,			NULL														},
};

CCodeGen_Arm::CCodeGen_Arm()
: m_stream(nullptr)
{
	for(auto* constMatcher = g_constMatchers; constMatcher->emitter != nullptr; constMatcher++)
	{
		MATCHER matcher;
		matcher.op			= constMatcher->op;
		matcher.dstType		= constMatcher->dstType;
		matcher.src1Type	= constMatcher->src1Type;
		matcher.src2Type	= constMatcher->src2Type;
		matcher.emitter		= std::bind(constMatcher->emitter, this, std::placeholders::_1);
		m_matchers.insert(MatcherMapType::value_type(matcher.op, matcher));
	}

	for(auto* constMatcher = g_64ConstMatchers; constMatcher->emitter != nullptr; constMatcher++)
	{
		MATCHER matcher;
		matcher.op			= constMatcher->op;
		matcher.dstType		= constMatcher->dstType;
		matcher.src1Type	= constMatcher->src1Type;
		matcher.src2Type	= constMatcher->src2Type;
		matcher.emitter		= std::bind(constMatcher->emitter, this, std::placeholders::_1);
		m_matchers.insert(MatcherMapType::value_type(matcher.op, matcher));
	}

	for(auto* constMatcher = g_fpuConstMatchers; constMatcher->emitter != nullptr; constMatcher++)
	{
		MATCHER matcher;
		matcher.op			= constMatcher->op;
		matcher.dstType		= constMatcher->dstType;
		matcher.src1Type	= constMatcher->src1Type;
		matcher.src2Type	= constMatcher->src2Type;
		matcher.emitter		= std::bind(constMatcher->emitter, this, std::placeholders::_1);
		m_matchers.insert(MatcherMapType::value_type(matcher.op, matcher));
	}

	for(auto* constMatcher = g_mdConstMatchers; constMatcher->emitter != nullptr; constMatcher++)
	{
		MATCHER matcher;
		matcher.op			= constMatcher->op;
		matcher.dstType		= constMatcher->dstType;
		matcher.src1Type	= constMatcher->src1Type;
		matcher.src2Type	= constMatcher->src2Type;
		matcher.emitter		= std::bind(constMatcher->emitter, this, std::placeholders::_1);
		m_matchers.insert(MatcherMapType::value_type(matcher.op, matcher));
	}
}

CCodeGen_Arm::~CCodeGen_Arm()
{

}

unsigned int CCodeGen_Arm::GetAvailableRegisterCount() const
{
	return MAX_REGISTERS;
}

unsigned int CCodeGen_Arm::GetAvailableMdRegisterCount() const
{
	return 0;
}

unsigned int CCodeGen_Arm::GetAddressSize() const
{
	return 4;
}

bool CCodeGen_Arm::CanHold128BitsReturnValueInRegisters() const
{
	return false;
}

void CCodeGen_Arm::SetStream(Framework::CStream* stream)
{
	m_stream = stream;
	m_assembler.SetStream(stream);
}

void CCodeGen_Arm::RegisterExternalSymbols(CObjectFile* objectFile) const
{
	objectFile->AddExternalSymbol("_CodeGen_Arm_div_unsigned",	reinterpret_cast<void*>(&CodeGen_Arm_div_unsigned));
	objectFile->AddExternalSymbol("_CodeGen_Arm_div_signed",	reinterpret_cast<void*>(&CodeGen_Arm_div_signed));
	objectFile->AddExternalSymbol("_CodeGen_Arm_mod_unsigned",	reinterpret_cast<void*>(&CodeGen_Arm_mod_unsigned));
	objectFile->AddExternalSymbol("_CodeGen_Arm_mod_signed",	reinterpret_cast<void*>(&CodeGen_Arm_mod_signed));
}

void CCodeGen_Arm::GenerateCode(const StatementList& statements, unsigned int stackSize)
{
	uint16 registerSave = GetSavedRegisterList(GetRegisterUsage(statements));

	Emit_Prolog(stackSize, registerSave);

	for(const auto& statement : statements)
	{
		bool found = false;
		auto begin = m_matchers.lower_bound(statement.op);
		auto end = m_matchers.upper_bound(statement.op);

		for(auto matchIterator(begin); matchIterator != end; matchIterator++)
		{
			const MATCHER& matcher(matchIterator->second);
			if(!SymbolMatches(matcher.dstType, statement.dst)) continue;
			if(!SymbolMatches(matcher.src1Type, statement.src1)) continue;
			if(!SymbolMatches(matcher.src2Type, statement.src2)) continue;
			matcher.emitter(statement);
			found = true;
			break;
		}
		assert(found);
		if(!found)
		{
			throw std::runtime_error("No suitable emitter found for statement.");
		}
	}

	Emit_Epilog(stackSize, registerSave);

	m_assembler.ResolveLabelReferences();
	m_assembler.ClearLabels();
	m_labels.clear();
}

uint16 CCodeGen_Arm::GetSavedRegisterList(uint32 registerUsage)
{
	uint16 registerSave = 0;
	for(unsigned int i = 0; i < MAX_REGISTERS; i++)
	{
		if((1 << i) & registerUsage)
		{
			registerSave |= (1 << g_registers[i]);
		}
	}
	registerSave |= (1 << g_callAddressRegister);
	registerSave |= (1 << g_baseRegister);
	registerSave |= (1 << CArmAssembler::rLR);
	return registerSave;
}

void CCodeGen_Arm::Emit_Prolog(unsigned int stackSize, uint16 registerSave)
{
	m_assembler.Stmdb(CArmAssembler::rSP, registerSave);
	if(stackSize != 0)
	{
		m_assembler.Sub(CArmAssembler::rSP, CArmAssembler::rSP, CArmAssembler::MakeImmediateAluOperand(stackSize, 0));
	}
	m_assembler.Mov(CArmAssembler::r11, CArmAssembler::r0);
	m_stackLevel = 0;
}

void CCodeGen_Arm::Emit_Epilog(unsigned int stackSize, uint16 registerSave)
{
	if(stackSize != 0)
	{
		m_assembler.Add(CArmAssembler::rSP, CArmAssembler::rSP, CArmAssembler::MakeImmediateAluOperand(stackSize, 0));
	}
	m_assembler.Ldmia(CArmAssembler::rSP, registerSave);
	m_assembler.Bx(CArmAssembler::rLR);
}

uint32 CCodeGen_Arm::RotateRight(uint32 value)
{
	uint32 carry = value & 1;
	value >>= 1;
	value |= carry << 31;
	return value;
}

uint32 CCodeGen_Arm::RotateLeft(uint32 value)
{
	uint32 carry = value >> 31;
	value <<= 1;
	value |= carry;
	return value;
}

bool CCodeGen_Arm::TryGetAluImmediateParams(uint32 constant, uint8& immediate, uint8& shiftAmount)
{
	uint32 shadowConstant = constant;
	shiftAmount = 0xFF;
	
	for(unsigned int i = 0; i < 16; i++)
	{
		if((shadowConstant & 0xFF) == shadowConstant)
		{
			shiftAmount = i;
			break;
		}
		shadowConstant = RotateLeft(shadowConstant);
		shadowConstant = RotateLeft(shadowConstant);
	}
	
	if(shiftAmount != 0xFF)
	{
		immediate = static_cast<uint8>(shadowConstant);
		return true;
	}
	else
	{
		return false;
	}
}

void CCodeGen_Arm::LoadConstantInRegister(CArmAssembler::REGISTER registerId, uint32 constant, bool relocatable)
{	
	if(!relocatable)
	{
		//Try normal move
		{
			uint8 immediate = 0;
			uint8 shiftAmount = 0;
			if(TryGetAluImmediateParams(constant, immediate, shiftAmount))
			{
				m_assembler.Mov(registerId, CArmAssembler::MakeImmediateAluOperand(immediate, shiftAmount));
				return;
			}
		}
	
		//Try not move
		{
			uint8 immediate = 0;
			uint8 shiftAmount = 0;
			if(TryGetAluImmediateParams(~constant, immediate, shiftAmount))
			{
				m_assembler.Mvn(registerId, CArmAssembler::MakeImmediateAluOperand(immediate, shiftAmount));
				return;
			}
		}
	}
		
	//Otherwise, use paired move
	m_assembler.Movw(registerId, static_cast<uint16>(constant & 0xFFFF));
	if(relocatable || ((constant & 0xFFFF0000) != 0))
	{
		m_assembler.Movt(registerId, static_cast<uint16>(constant >> 16));
	}

	if(relocatable && m_externalSymbolReferencedHandler)
	{
		auto position = m_stream->GetLength();
		m_externalSymbolReferencedHandler(reinterpret_cast<void*>(constant), position - 8);
	}
}

void CCodeGen_Arm::LoadMemoryInRegister(CArmAssembler::REGISTER registerId, CSymbol* src)
{
	switch(src->m_type)
	{
	case SYM_RELATIVE:
		LoadRelativeInRegister(registerId, src);
		break;
	case SYM_TEMPORARY:
		LoadTemporaryInRegister(registerId, src);
		break;
	default:
		assert(0);
		break;
	}
}

void CCodeGen_Arm::StoreRegisterInMemory(CSymbol* dst, CArmAssembler::REGISTER registerId)
{
	switch(dst->m_type)
	{
	case SYM_RELATIVE:
		StoreRegisterInRelative(dst, registerId);
		break;
	case SYM_TEMPORARY:
		StoreRegisterInTemporary(dst, registerId);
		break;
	default:
		assert(0);
		break;
	}
}

void CCodeGen_Arm::LoadRelativeInRegister(CArmAssembler::REGISTER registerId, CSymbol* src)
{
	assert(src->m_type == SYM_RELATIVE);
	assert((src->m_valueLow & 0x03) == 0x00);
	m_assembler.Ldr(registerId, g_baseRegister, CArmAssembler::MakeImmediateLdrAddress(src->m_valueLow));
}

void CCodeGen_Arm::StoreRegisterInRelative(CSymbol* dst, CArmAssembler::REGISTER registerId)
{
	assert(dst->m_type == SYM_RELATIVE);
	assert((dst->m_valueLow & 0x03) == 0x00);
	m_assembler.Str(registerId, g_baseRegister, CArmAssembler::MakeImmediateLdrAddress(dst->m_valueLow));
}

void CCodeGen_Arm::LoadTemporaryInRegister(CArmAssembler::REGISTER registerId, CSymbol* src)
{
	assert(src->m_type == SYM_TEMPORARY);
	m_assembler.Ldr(registerId, CArmAssembler::rSP, CArmAssembler::MakeImmediateLdrAddress(src->m_stackLocation + m_stackLevel));
}

void CCodeGen_Arm::StoreRegisterInTemporary(CSymbol* dst, CArmAssembler::REGISTER registerId)
{
	assert(dst->m_type == SYM_TEMPORARY);
	m_assembler.Str(registerId, CArmAssembler::rSP, CArmAssembler::MakeImmediateLdrAddress(dst->m_stackLocation + m_stackLevel));
}

void CCodeGen_Arm::LoadRelativeReferenceInRegister(CArmAssembler::REGISTER registerId, CSymbol* src)
{
	assert(src->m_type == SYM_REL_REFERENCE);
	assert((src->m_valueLow & 0x03) == 0x00);
	m_assembler.Ldr(registerId, g_baseRegister, CArmAssembler::MakeImmediateLdrAddress(src->m_valueLow));
}

void CCodeGen_Arm::LoadTemporaryReferenceInRegister(CArmAssembler::REGISTER registerId, CSymbol* src)
{
	assert(src->m_type == SYM_TMP_REFERENCE);
	m_assembler.Ldr(registerId, CArmAssembler::rSP, CArmAssembler::MakeImmediateLdrAddress(src->m_stackLocation + m_stackLevel));
}

void CCodeGen_Arm::StoreInRegisterTemporaryReference(CSymbol* dst, CArmAssembler::REGISTER registerId)
{
	assert(dst->m_type == SYM_TMP_REFERENCE);
	m_assembler.Str(registerId, CArmAssembler::rSP, CArmAssembler::MakeImmediateLdrAddress(dst->m_stackLocation + m_stackLevel));
}

CArmAssembler::AluLdrShift CCodeGen_Arm::GetAluShiftFromSymbol(CArmAssembler::SHIFT shiftType, CSymbol* symbol, CArmAssembler::REGISTER preferedRegister)
{
	switch(symbol->m_type)
	{
	case SYM_REGISTER:
		return CArmAssembler::MakeVariableShift(shiftType, g_registers[symbol->m_valueLow]);
		break;
	case SYM_TEMPORARY:
	case SYM_RELATIVE:
		LoadMemoryInRegister(preferedRegister, symbol);
		return CArmAssembler::MakeVariableShift(shiftType, preferedRegister);
		break;
	case SYM_CONSTANT:
		return CArmAssembler::MakeConstantShift(shiftType, static_cast<uint8>(symbol->m_valueLow));
		break;
	default:
		throw std::runtime_error("Invalid symbol type.");
		break;
	}
}

CArmAssembler::REGISTER CCodeGen_Arm::PrepareSymbolRegisterDef(CSymbol* symbol, CArmAssembler::REGISTER preferedRegister)
{
	switch(symbol->m_type)
	{
	case SYM_REGISTER:
		return g_registers[symbol->m_valueLow];
		break;
	case SYM_TEMPORARY:
	case SYM_RELATIVE:
		return preferedRegister;
		break;
	default:
		throw std::runtime_error("Invalid symbol type.");
		break;
	}
}

CArmAssembler::REGISTER CCodeGen_Arm::PrepareSymbolRegisterUse(CSymbol* symbol, CArmAssembler::REGISTER preferedRegister)
{
	switch(symbol->m_type)
	{
	case SYM_REGISTER:
		return g_registers[symbol->m_valueLow];
		break;
	case SYM_TEMPORARY:
	case SYM_RELATIVE:
		LoadMemoryInRegister(preferedRegister, symbol);
		return preferedRegister;
		break;
	case SYM_CONSTANT:
		LoadConstantInRegister(preferedRegister, symbol->m_valueLow);
		return preferedRegister;
		break;
	default:
		throw std::runtime_error("Invalid symbol type.");
		break;
	}
}

void CCodeGen_Arm::CommitSymbolRegister(CSymbol* symbol, CArmAssembler::REGISTER usedRegister)
{
	switch(symbol->m_type)
	{
	case SYM_REGISTER:
		assert(usedRegister == g_registers[symbol->m_valueLow]);
		break;
	case SYM_TEMPORARY:
	case SYM_RELATIVE:
		StoreRegisterInMemory(symbol, usedRegister);
		break;
	default:
		throw std::runtime_error("Invalid symbol type.");
		break;
	}
}

void CCodeGen_Arm::AlignParam64(PARAM_STATE& paramState)
{
	if(paramState.index & 1)
	{
		paramState.index++;
	}
}

CArmAssembler::REGISTER CCodeGen_Arm::PrepareParam(const PARAM_STATE& paramState)
{
	assert(paramState.index < MAX_PARAMS);
	return g_paramRegs[paramState.index];
}

void CCodeGen_Arm::CommitParam(PARAM_STATE& paramState)
{
	assert(paramState.index < MAX_PARAMS);
	paramState.index++;
}

CArmAssembler::LABEL CCodeGen_Arm::GetLabel(uint32 blockId)
{
	CArmAssembler::LABEL result;
	LabelMapType::const_iterator labelIterator(m_labels.find(blockId));
	if(labelIterator == m_labels.end())
	{
		result = m_assembler.CreateLabel();
		m_labels[blockId] = result;
	}
	else
	{
		result = labelIterator->second;
	}
	return result;
}

void CCodeGen_Arm::MarkLabel(const STATEMENT& statement)
{
	CArmAssembler::LABEL label = GetLabel(statement.jmpBlock);
	m_assembler.MarkLabel(label);
}

void CCodeGen_Arm::Emit_Nop(const STATEMENT& statement)
{
	
}

void CCodeGen_Arm::Emit_Param_Ctx(const STATEMENT& statement)
{
	auto src1 = statement.src1->GetSymbol().get();

	assert(src1->m_type == SYM_CONTEXT);
	
	m_params.push_back(
		[this] (PARAM_STATE& paramState)
		{
			auto paramReg = PrepareParam(paramState);
			m_assembler.Mov(paramReg, g_baseRegister);
			CommitParam(paramState);
		}
	);
}

void CCodeGen_Arm::Emit_Param_Reg(const STATEMENT& statement)
{
	auto src1 = statement.src1->GetSymbol().get();

	assert(src1->m_type == SYM_REGISTER);
	
	m_params.push_back(
		[this, src1] (PARAM_STATE& paramState)
		{
			auto paramReg = PrepareParam(paramState);
			m_assembler.Mov(paramReg, g_registers[src1->m_valueLow]);
			CommitParam(paramState);
		}
	);
}

void CCodeGen_Arm::Emit_Param_Mem(const STATEMENT& statement)
{
	auto src1 = statement.src1->GetSymbol().get();
		
	m_params.push_back(
		[this, src1] (PARAM_STATE& paramState)
		{
			auto paramReg = PrepareParam(paramState);
			LoadMemoryInRegister(paramReg, src1);
			CommitParam(paramState);
		}
	);
}

void CCodeGen_Arm::Emit_Param_Cst(const STATEMENT& statement)
{
	auto src1 = statement.src1->GetSymbol().get();
	
	assert(src1->m_type == SYM_CONSTANT);
	
	m_params.push_back(
		[this, src1] (PARAM_STATE& paramState)
		{
			auto paramReg = PrepareParam(paramState);
			LoadConstantInRegister(paramReg, src1->m_valueLow, false);
			CommitParam(paramState);
		}
	);
}

void CCodeGen_Arm::Emit_Param_Mem64(const STATEMENT& statement)
{
	auto src1 = statement.src1->GetSymbol().get();

	m_params.push_back(
		[this, src1] (PARAM_STATE& paramState)
		{
			AlignParam64(paramState);
			{
				auto paramReg = PrepareParam(paramState);
				LoadMemory64LowInRegister(paramReg, src1);
				CommitParam(paramState);
			}
			{
				auto paramReg = PrepareParam(paramState);
				LoadMemory64HighInRegister(paramReg, src1);
				CommitParam(paramState);
			}
		}
	);
}

void CCodeGen_Arm::Emit_Param_Cst64(const STATEMENT& statement)
{
	auto src1 = statement.src1->GetSymbol().get();

	m_params.push_back(
		[this, src1] (PARAM_STATE& paramState)
		{
			AlignParam64(paramState);
			{
				auto paramReg = PrepareParam(paramState);
				LoadConstantInRegister(paramReg, src1->m_valueLow, false);
				CommitParam(paramState);
			}
			{
				auto paramReg = PrepareParam(paramState);
				LoadConstantInRegister(paramReg, src1->m_valueHigh, false);
				CommitParam(paramState);
			}
		}
	);
}

void CCodeGen_Arm::Emit_Param_Mem128(const STATEMENT& statement)
{
	auto src1 = statement.src1->GetSymbol().get();

	m_params.push_back(
		[this, src1] (PARAM_STATE& paramState)
		{
			auto paramReg = PrepareParam(paramState);
			LoadMemory128AddressInRegister(paramReg, src1);
			CommitParam(paramState);
		}
	);
}

void CCodeGen_Arm::Emit_ParamRet_Tmp128(const STATEMENT& statement)
{
	Emit_Param_Mem128(statement);
}

void CCodeGen_Arm::Emit_Call(const STATEMENT& statement)
{
	auto src1 = statement.src1->GetSymbol().get();
	auto src2 = statement.src2->GetSymbol().get();
	
	assert(src1->m_type == SYM_CONSTANT);
	assert(src2->m_type == SYM_CONSTANT);

	unsigned int paramCount = src2->m_valueLow;
	PARAM_STATE paramState;

	for(unsigned int i = 0; i < paramCount; i++)
	{
		auto emitter(m_params.back());
		m_params.pop_back();
		emitter(paramState);
	}

	//No value should be saved in r4 at this point (register is spilled before)
	LoadConstantInRegister(g_callAddressRegister, src1->m_valueLow, true);
	m_assembler.Mov(CArmAssembler::rLR, CArmAssembler::rPC);
	m_assembler.Mov(CArmAssembler::rPC, g_callAddressRegister);
}

void CCodeGen_Arm::Emit_RetVal_Reg(const STATEMENT& statement)
{	
	CSymbol* dst = statement.dst->GetSymbol().get();
	
	assert(dst->m_type == SYM_REGISTER);
	
	m_assembler.Mov(g_registers[dst->m_valueLow], CArmAssembler::r0);
}

void CCodeGen_Arm::Emit_RetVal_Tmp(const STATEMENT& statement)
{	
	CSymbol* dst = statement.dst->GetSymbol().get();
	
	assert(dst->m_type == SYM_TEMPORARY);
	
	StoreRegisterInTemporary(dst, CArmAssembler::r0);
}

void CCodeGen_Arm::Emit_RetVal_Mem64(const STATEMENT& statement)
{
	auto dst = statement.dst->GetSymbol().get();

	StoreRegisterInMemory64Low(dst, CArmAssembler::r0);
	StoreRegisterInMemory64High(dst, CArmAssembler::r1);
}

void CCodeGen_Arm::Emit_Mov_RegReg(const STATEMENT& statement)
{
	auto dst = statement.dst->GetSymbol().get();
	auto src1 = statement.src1->GetSymbol().get();

	m_assembler.Mov(g_registers[dst->m_valueLow], g_registers[src1->m_valueLow]);
}

void CCodeGen_Arm::Emit_Mov_RegMem(const STATEMENT& statement)
{
	auto dst = statement.dst->GetSymbol().get();
	auto src1 = statement.src1->GetSymbol().get();

	LoadMemoryInRegister(g_registers[dst->m_valueLow], src1);
}

void CCodeGen_Arm::Emit_Mov_RegCst(const STATEMENT& statement)
{
	auto dst = statement.dst->GetSymbol().get();
	auto src1 = statement.src1->GetSymbol().get();

	assert(dst->m_type  == SYM_REGISTER);
	assert(src1->m_type == SYM_CONSTANT);

	LoadConstantInRegister(g_registers[dst->m_valueLow], src1->m_valueLow);
}

void CCodeGen_Arm::Emit_Mov_MemReg(const STATEMENT& statement)
{
	auto dst = statement.dst->GetSymbol().get();
	auto src1 = statement.src1->GetSymbol().get();

	assert(src1->m_type == SYM_REGISTER);

	StoreRegisterInMemory(dst, g_registers[src1->m_valueLow]);
}

void CCodeGen_Arm::Emit_Mov_MemMem(const STATEMENT& statement)
{
	auto dst = statement.dst->GetSymbol().get();
	auto src1 = statement.src1->GetSymbol().get();
	
	auto tmpReg = CArmAssembler::r0;
	LoadMemoryInRegister(tmpReg, src1);
	StoreRegisterInMemory(dst, tmpReg);
}

void CCodeGen_Arm::Emit_Mov_MemCst(const STATEMENT& statement)
{
	auto dst = statement.dst->GetSymbol().get();
	auto src1 = statement.src1->GetSymbol().get();
	
	assert(src1->m_type == SYM_CONSTANT);
	
	auto tmpReg = CArmAssembler::r0;
	LoadConstantInRegister(tmpReg, src1->m_valueLow);
	StoreRegisterInMemory(dst, tmpReg);
}

void CCodeGen_Arm::Emit_Jmp(const STATEMENT& statement)
{
	m_assembler.BCc(CArmAssembler::CONDITION_AL, GetLabel(statement.jmpBlock));
}

void CCodeGen_Arm::Emit_CondJmp(const STATEMENT& statement)
{
	CArmAssembler::LABEL label(GetLabel(statement.jmpBlock));
	
	switch(statement.jmpCondition)
	{
		case CONDITION_EQ:
			m_assembler.BCc(CArmAssembler::CONDITION_EQ, label);
			break;
		case CONDITION_NE:
			m_assembler.BCc(CArmAssembler::CONDITION_NE, label);
			break;
		case CONDITION_LE:
			m_assembler.BCc(CArmAssembler::CONDITION_LE, label);
			break;
		case CONDITION_GT:
			m_assembler.BCc(CArmAssembler::CONDITION_GT, label);
			break;
		default:
			assert(0);
			break;
	}
}

void CCodeGen_Arm::Emit_CondJmp_VarVar(const STATEMENT& statement)
{
	auto src1 = statement.src1->GetSymbol().get();
	auto src2 = statement.src2->GetSymbol().get();
	
	assert(src2->m_type != SYM_CONSTANT);	//We can do better if we have a constant

	auto src1Reg = PrepareSymbolRegisterUse(src1, CArmAssembler::r1);
	auto src2Reg = PrepareSymbolRegisterUse(src2, CArmAssembler::r2);
	m_assembler.Cmp(src1Reg, src2Reg);
	Emit_CondJmp(statement);
}

void CCodeGen_Arm::Emit_CondJmp_VarCst(const STATEMENT& statement)
{
	auto src1 = statement.src1->GetSymbol().get();
	auto src2 = statement.src2->GetSymbol().get();
	
	assert(src2->m_type == SYM_CONSTANT);
	
	auto src1Reg = PrepareSymbolRegisterUse(src1, CArmAssembler::r1);
	Cmp_GenericRegCst(src1Reg, src2->m_valueLow, CArmAssembler::r2);
	Emit_CondJmp(statement);
}

void CCodeGen_Arm::Cmp_GetFlag(CArmAssembler::REGISTER registerId, Jitter::CONDITION condition)
{
	CArmAssembler::ImmediateAluOperand falseOperand(CArmAssembler::MakeImmediateAluOperand(0, 0));
	CArmAssembler::ImmediateAluOperand trueOperand(CArmAssembler::MakeImmediateAluOperand(1, 0));
	switch(condition)
	{
		case CONDITION_EQ:
			m_assembler.MovCc(CArmAssembler::CONDITION_NE, registerId, falseOperand);
			m_assembler.MovCc(CArmAssembler::CONDITION_EQ, registerId, trueOperand);
			break;
		case CONDITION_NE:
			m_assembler.MovCc(CArmAssembler::CONDITION_EQ, registerId, falseOperand);
			m_assembler.MovCc(CArmAssembler::CONDITION_NE, registerId, trueOperand);
			break;
		case CONDITION_LT:
			m_assembler.MovCc(CArmAssembler::CONDITION_GE, registerId, falseOperand);
			m_assembler.MovCc(CArmAssembler::CONDITION_LT, registerId, trueOperand);
			break;
		case CONDITION_GT:
			m_assembler.MovCc(CArmAssembler::CONDITION_LE, registerId, falseOperand);
			m_assembler.MovCc(CArmAssembler::CONDITION_GT, registerId, trueOperand);
			break;
		case CONDITION_BL:
			m_assembler.MovCc(CArmAssembler::CONDITION_CS, registerId, falseOperand);
			m_assembler.MovCc(CArmAssembler::CONDITION_CC, registerId, trueOperand);
			break;
		case CONDITION_AB:
			m_assembler.MovCc(CArmAssembler::CONDITION_LS, registerId, falseOperand);
			m_assembler.MovCc(CArmAssembler::CONDITION_HI, registerId, trueOperand);
			break;
		default:
			assert(0);
			break;
	}
}

void CCodeGen_Arm::Cmp_GenericRegCst(CArmAssembler::REGISTER src1Reg, uint32 src2, CArmAssembler::REGISTER src2Reg)
{
	uint8 immediate = 0;
	uint8 shiftAmount = 0;
	if(TryGetAluImmediateParams(src2, immediate, shiftAmount))
	{
		m_assembler.Cmp(src1Reg, CArmAssembler::MakeImmediateAluOperand(immediate, shiftAmount));
	}
	else if(TryGetAluImmediateParams(-static_cast<int32>(src2), immediate, shiftAmount))
	{
		m_assembler.Cmn(src1Reg, CArmAssembler::MakeImmediateAluOperand(immediate, shiftAmount));
	}
	else
	{
		assert(src1Reg != src2Reg);
		LoadConstantInRegister(src2Reg, src2);
		m_assembler.Cmp(src1Reg, src2Reg);
	}
}

void CCodeGen_Arm::Emit_Cmp_AnyAnyAny(const STATEMENT& statement)
{
	auto dst = statement.dst->GetSymbol().get();
	auto src1 = statement.src1->GetSymbol().get();
	auto src2 = statement.src2->GetSymbol().get();

	auto dstReg = PrepareSymbolRegisterDef(dst, CArmAssembler::r0);
	auto src1Reg = PrepareSymbolRegisterUse(src1, CArmAssembler::r1);
	auto src2Reg = PrepareSymbolRegisterUse(src2, CArmAssembler::r2);

	m_assembler.Cmp(src1Reg, src2Reg);
	Cmp_GetFlag(dstReg, statement.jmpCondition);
	CommitSymbolRegister(dst, dstReg);
}

void CCodeGen_Arm::Emit_Cmp_AnyAnyCst(const STATEMENT& statement)
{
	auto dst = statement.dst->GetSymbol().get();
	auto src1 = statement.src1->GetSymbol().get();
	auto src2 = statement.src2->GetSymbol().get();

	assert(src2->m_type == SYM_CONSTANT);

	auto dstReg = PrepareSymbolRegisterDef(dst, CArmAssembler::r0);
	auto src1Reg = PrepareSymbolRegisterUse(src1, CArmAssembler::r1);
	auto cst = src2->m_valueLow;

	Cmp_GenericRegCst(src1Reg, cst, CArmAssembler::r2);
	Cmp_GetFlag(dstReg, statement.jmpCondition);
	CommitSymbolRegister(dst, dstReg);
}

void CCodeGen_Arm::Emit_Not_RegReg(const STATEMENT& statement)
{
	auto dst = statement.dst->GetSymbol().get();
	auto src1 = statement.src1->GetSymbol().get();

	assert(dst->m_type  == SYM_REGISTER);
	assert(src1->m_type == SYM_REGISTER);

	m_assembler.Mvn(g_registers[dst->m_valueLow], g_registers[src1->m_valueLow]);
}

void CCodeGen_Arm::Emit_Not_MemReg(const STATEMENT& statement)
{
	auto dst = statement.dst->GetSymbol().get();
	auto src1 = statement.src1->GetSymbol().get();
	
	assert(src1->m_type == SYM_REGISTER);
	
	auto dstReg = CArmAssembler::r1;
	m_assembler.Mvn(dstReg, g_registers[src1->m_valueLow]);
	StoreRegisterInMemory(dst, dstReg);
}

void CCodeGen_Arm::Emit_Not_MemMem(const STATEMENT& statement)
{
	auto dst = statement.dst->GetSymbol().get();
	auto src1 = statement.src1->GetSymbol().get();

	auto srcReg = CArmAssembler::r0;
	auto dstReg = CArmAssembler::r1;
	LoadMemoryInRegister(srcReg, src1);
	m_assembler.Mvn(dstReg, srcReg);
	StoreRegisterInMemory(dst, dstReg);
}

void CCodeGen_Arm::Emit_AddRef_TmpRelReg(const STATEMENT& statement)
{
	CSymbol* dst = statement.dst->GetSymbol().get();
	CSymbol* src1 = statement.src1->GetSymbol().get();
	CSymbol* src2 = statement.src2->GetSymbol().get();
	
	assert(dst->m_type  == SYM_TMP_REFERENCE);
	assert(src1->m_type == SYM_REL_REFERENCE);
	assert(src2->m_type == SYM_REGISTER);
	
	CArmAssembler::REGISTER tmpReg = CArmAssembler::r0;
	
	LoadRelativeReferenceInRegister(tmpReg, src1);
	m_assembler.Add(tmpReg, tmpReg, g_registers[src2->m_valueLow]);
	StoreInRegisterTemporaryReference(dst, tmpReg);
}

void CCodeGen_Arm::Emit_AddRef_TmpRelCst(const STATEMENT& statement)
{
	CSymbol* dst = statement.dst->GetSymbol().get();
	CSymbol* src1 = statement.src1->GetSymbol().get();
	CSymbol* src2 = statement.src2->GetSymbol().get();
	
	assert(dst->m_type  == SYM_TMP_REFERENCE);
	assert(src1->m_type == SYM_REL_REFERENCE);
	assert(src2->m_type == SYM_CONSTANT);
	
	CArmAssembler::REGISTER tmpReg0 = CArmAssembler::r0;
	CArmAssembler::REGISTER tmpReg1 = CArmAssembler::r1;
	
	LoadRelativeReferenceInRegister(tmpReg0, src1);
	LoadConstantInRegister(tmpReg1, src2->m_valueLow);
	m_assembler.Add(tmpReg0, tmpReg0, tmpReg1);
	StoreInRegisterTemporaryReference(dst, tmpReg0);
}

void CCodeGen_Arm::Emit_LoadFromRef_RegTmp(const STATEMENT& statement)
{
	CSymbol* dst = statement.dst->GetSymbol().get();
	CSymbol* src1 = statement.src1->GetSymbol().get();
	
	assert(dst->m_type  == SYM_REGISTER);
	assert(src1->m_type == SYM_TMP_REFERENCE);
	
	CArmAssembler::REGISTER addressReg = CArmAssembler::r0;
	LoadTemporaryReferenceInRegister(addressReg, src1);
	m_assembler.Ldr(g_registers[dst->m_valueLow], addressReg, CArmAssembler::MakeImmediateLdrAddress(0));
}

void CCodeGen_Arm::Emit_StoreAtRef_TmpReg(const STATEMENT& statement)
{
	CSymbol* src1 = statement.src1->GetSymbol().get();
	CSymbol* src2 = statement.src2->GetSymbol().get();
	
	assert(src1->m_type == SYM_TMP_REFERENCE);
	assert(src2->m_type == SYM_REGISTER);
	
	CArmAssembler::REGISTER addressReg = CArmAssembler::r0;
	
	LoadTemporaryReferenceInRegister(addressReg, src1);
	m_assembler.Str(g_registers[src2->m_valueLow], addressReg, CArmAssembler::MakeImmediateLdrAddress(0));
}

void CCodeGen_Arm::Emit_StoreAtRef_TmpRel(const STATEMENT& statement)
{
	CSymbol* src1 = statement.src1->GetSymbol().get();
	CSymbol* src2 = statement.src2->GetSymbol().get();
	
	assert(src1->m_type == SYM_TMP_REFERENCE);
	assert(src2->m_type == SYM_RELATIVE);
	
	CArmAssembler::REGISTER addressReg = CArmAssembler::r0;
	CArmAssembler::REGISTER valueReg = CArmAssembler::r1;
	
	LoadTemporaryReferenceInRegister(addressReg, src1);
	LoadRelativeInRegister(valueReg, src2);
	m_assembler.Str(valueReg, addressReg, CArmAssembler::MakeImmediateLdrAddress(0));
}

void CCodeGen_Arm::Emit_StoreAtRef_TmpCst(const STATEMENT& statement)
{
	CSymbol* src1 = statement.src1->GetSymbol().get();
	CSymbol* src2 = statement.src2->GetSymbol().get();
	
	assert(src1->m_type == SYM_TMP_REFERENCE);
	assert(src2->m_type == SYM_CONSTANT);
	
	CArmAssembler::REGISTER addressReg = CArmAssembler::r0;
	CArmAssembler::REGISTER valueReg = CArmAssembler::r1;
	
	LoadTemporaryReferenceInRegister(addressReg, src1);
	LoadConstantInRegister(valueReg, src2->m_valueLow);
	m_assembler.Str(valueReg, addressReg, CArmAssembler::MakeImmediateLdrAddress(0));
}
