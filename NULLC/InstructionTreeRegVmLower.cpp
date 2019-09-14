#include "InstructionTreeRegVmLower.h"

#include "ExpressionTree.h"
#include "InstructionTreeVm.h"
#include "InstructionTreeVmCommon.h"

namespace
{
	bool IsNonLocalValue(VmInstruction *inst)
	{
		if(inst->users.empty())
			return false;

		for(unsigned i = 0; i < inst->users.size(); i++)
		{
			VmInstruction *user = getType<VmInstruction>(inst->users[i]);

			if(inst->parent != user->parent)
				return true;
		}

		return false;
	}
}

void RegVmLoweredBlock::AddInstruction(ExpressionContext &ctx, RegVmLoweredInstruction* instruction)
{
	(void)ctx;

	assert(instruction);
	assert(instruction->parent == NULL);
	assert(instruction->prevSibling == NULL);
	assert(instruction->nextSibling == NULL);

	instruction->parent = this;

	if(instruction->argument && instruction->argument->container)
		instruction->argument->container->regVmUsers.push_back(instruction);

	if(!firstInstruction)
	{
		firstInstruction = lastInstruction = instruction;
	}
	else
	{
		lastInstruction->nextSibling = instruction;
		instruction->prevSibling = lastInstruction;

		lastInstruction = instruction;
	}
}

void RegVmLoweredBlock::AddInstruction(ExpressionContext &ctx, SynBase *location, RegVmInstructionCode code)
{
	AddInstruction(ctx, new (ctx.get<RegVmLoweredInstruction>()) RegVmLoweredInstruction(ctx.allocator, location, code, 0, 0, 0, NULL));
}

void RegVmLoweredBlock::AddInstruction(ExpressionContext &ctx, SynBase *location, RegVmInstructionCode code, unsigned char rA, unsigned char rB, unsigned char rC)
{
	AddInstruction(ctx, new (ctx.get<RegVmLoweredInstruction>()) RegVmLoweredInstruction(ctx.allocator, location, code, rA, rB, rC, NULL));
}

void RegVmLoweredBlock::AddInstruction(ExpressionContext &ctx, SynBase *location, RegVmInstructionCode code, unsigned char rA, unsigned char rB, unsigned char rC, VmConstant *argument)
{
	AddInstruction(ctx, new (ctx.get<RegVmLoweredInstruction>()) RegVmLoweredInstruction(ctx.allocator, location, code, rA, rB, rC, argument));
}

void RegVmLoweredBlock::AddInstruction(ExpressionContext &ctx, SynBase *location, RegVmInstructionCode code, unsigned char rA, unsigned char rB, unsigned char rC, unsigned argument)
{
	AddInstruction(ctx, new (ctx.get<RegVmLoweredInstruction>()) RegVmLoweredInstruction(ctx.allocator, location, code, rA, rB, rC, CreateConstantInt(ctx.allocator, NULL, argument)));
}

void RegVmLoweredBlock::AddInstruction(ExpressionContext &ctx, SynBase *location, RegVmInstructionCode code, unsigned char rA, unsigned char rB, unsigned char rC, VmBlock *argument)
{
	AddInstruction(ctx, new (ctx.get<RegVmLoweredInstruction>()) RegVmLoweredInstruction(ctx.allocator, location, code, rA, rB, rC, argument ? CreateConstantBlock(ctx.allocator, NULL, argument) : NULL));
}

void RegVmLoweredBlock::AddInstruction(ExpressionContext &ctx, SynBase *location, RegVmInstructionCode code, unsigned char rA, unsigned char rB, unsigned char rC, VmFunction *argument)
{
	AddInstruction(ctx, new (ctx.get<RegVmLoweredInstruction>()) RegVmLoweredInstruction(ctx.allocator, location, code, rA, rB, rC, argument ? CreateConstantFunction(ctx.allocator, NULL, argument) : NULL));
}

unsigned char RegVmLoweredFunction::GetRegister()
{
	unsigned char reg = 0;

	if(!freedRegisters.empty())
	{
		reg = freedRegisters.back();
		freedRegisters.pop_back();
	}
	else
	{
		// We start from rvrrCount register, so 0 means that we wrapped around from 255
		if(nextRegister == 0)
		{
			hasRegisterOverflow = true;
			return 255;
		}

		reg = nextRegister;
		nextRegister++;
	}

	assert(registerUsers[reg] == 0);
	registerUsers[reg]++;

	return reg;
}

void RegVmLoweredFunction::FreeRegister(unsigned char reg)
{
	assert(registerUsers[reg] == 1);
	registerUsers[reg]--;

	delayedFreedRegisters.push_back(reg);
}

void RegVmLoweredFunction::CompleteUse(VmValue *value)
{
	VmInstruction *instruction = getType<VmInstruction>(value);

	assert(instruction);

	// If the value is used across multiple blocks we can free register only if it's not in a set of liveOut values and this is the last use in the current block
	// TODO: try to actually do that ^
	if(IsNonLocalValue(instruction))
		return;

	// Phi instruction receive their register from live in values. Although it is still possible to free the register on its last use
	if(instruction->cmd == VM_INST_PHI)
		return;

	assert(instruction->regVmCompletedUsers < instruction->users.size());

	instruction->regVmCompletedUsers++;

	if(instruction->regVmCompletedUsers == instruction->users.size())
	{
		for(unsigned i = 0; i < instruction->regVmRegisters.size(); i++)
			FreeRegister(instruction->regVmRegisters[i]);
	}
}

unsigned char RegVmLoweredFunction::GetRegister(VmValue *value)
{
	VmInstruction *instruction = getType<VmInstruction>(value);

	CompleteUse(value);

	return instruction->regVmRegisters[0];
}

void RegVmLoweredFunction::GetRegisters(SmallArray<unsigned char, 32> &result, VmValue *value)
{
	VmInstruction *instruction = getType<VmInstruction>(value);

	CompleteUse(value);

	assert(!instruction->regVmRegisters.empty());

	for(unsigned i = 0; i < instruction->regVmRegisters.size(); i++)
		result.push_back(instruction->regVmRegisters[i]);
}

unsigned char RegVmLoweredFunction::AllocateRegister(VmValue *value, unsigned index, bool freeDelayed)
{
	VmInstruction *instruction = getType<VmInstruction>(value);

	if(freeDelayed)
		FreeDelayedRegisters(NULL);

	assert(instruction);
	assert(!instruction->users.empty());

	// Handle phi users, we might be forced to write to a different location
	for(unsigned i = 0; i < instruction->users.size(); i++)
	{
		if(VmInstruction *user = getType<VmInstruction>(instruction->users[i]))
		{
			if(user->cmd == VM_INST_PHI)
			{
				// Check if already allocated
				if(index < instruction->regVmRegisters.size())
					return instruction->regVmRegisters[index];

				// First value must have allocated registers, unless it's the current instruction
				VmInstruction *option = getType<VmInstruction>(user->arguments[0]);

				if(instruction != option && !instruction->regVmRegisters.empty())
				{
					unsigned regPos = instruction->regVmRegisters.size();

					unsigned char reg = option->regVmRegisters[regPos];

					assert(registerUsers[reg] == 1);
					instruction->regVmRegisters.push_back(reg);

					return reg;
				}

				break;
			}
		}
	}

	assert(instruction->regVmRegisters.size() == index);

	instruction->regVmRegisters.push_back(GetRegister());

	return instruction->regVmRegisters.back();
}

unsigned char RegVmLoweredFunction::GetRegisterForConstant()
{
	unsigned char result = GetRegister();

	constantRegisters.push_back(result);

	return result;
}

void RegVmLoweredFunction::FreeConstantRegisters()
{
	for(unsigned i = 0; i < constantRegisters.size(); i++)
		FreeRegister(constantRegisters[i]);

	constantRegisters.clear();
}

void RegVmLoweredFunction::FreeDelayedRegisters(RegVmLoweredBlock *lowBlock)
{
	for(unsigned i = 0; i < delayedFreedRegisters.size(); i++)
	{
		if(lowBlock)
			lowBlock->lastInstruction->postKillRegisters.push_back(delayedFreedRegisters[i]);
		else
			killedRegisters.push_back(delayedFreedRegisters[i]);

		assert(!freedRegisters.contains(delayedFreedRegisters[i]));
		freedRegisters.push_back(delayedFreedRegisters[i]);
	}

	delayedFreedRegisters.clear();
}

bool RegVmLoweredFunction::TransferRegisterTo(VmValue *value, unsigned char reg)
{
	VmInstruction *instruction = getType<VmInstruction>(value);

	assert(instruction);

	if(IsNonLocalValue(instruction))
		return false;

	for(unsigned i = 0; i < constantRegisters.size(); i++)
	{
		if(constantRegisters[i] == reg)
		{
			assert(registerUsers[reg] == 1);

			constantRegisters[i] = constantRegisters.back();
			constantRegisters.pop_back();

			instruction->regVmRegisters.push_back(reg);

			return true;
		}
	}

	for(unsigned i = 0; i < delayedFreedRegisters.size(); i++)
	{
		if(delayedFreedRegisters[i] == reg)
		{
			assert(registerUsers[reg] == 0);

			delayedFreedRegisters[i] = delayedFreedRegisters.back();
			delayedFreedRegisters.pop_back();

			instruction->regVmRegisters.push_back(reg);

			registerUsers[reg]++;

			return true;
		}
	}

	for(unsigned i = 0; i < freedRegisters.size(); i++)
	{
		if(freedRegisters[i] == reg)
		{
			assert(registerUsers[reg] == 0);

			freedRegisters[i] = freedRegisters.back();
			freedRegisters.pop_back();

			instruction->regVmRegisters.push_back(reg);

			registerUsers[reg]++;

			return true;
		}
	}

	return false;
}

void LowerConstantIntoBlock(ExpressionContext &ctx, RegVmLoweredFunction *lowFunction, RegVmLoweredBlock *lowBlock, SmallArray<unsigned char, 32> &result, VmValue *value)
{
	VmConstant *constant = getType<VmConstant>(value);

	assert(constant);

	if(constant->type == VmType::Int)
	{
		unsigned char targetReg = lowFunction->GetRegisterForConstant();

		lowBlock->AddInstruction(ctx, constant->source, rviLoadImm, targetReg, 0, 0, constant->iValue);

		result.push_back(targetReg);
	}
	else if(constant->type == VmType::Double)
	{
		unsigned data[2];
		memcpy(data, &constant->dValue, 8);

		unsigned char targetReg = lowFunction->GetRegisterForConstant();

		lowBlock->AddInstruction(ctx, constant->source, rviLoadImm, targetReg, 0, 0, data[0]);
		lowBlock->AddInstruction(ctx, constant->source, rviLoadImmDouble, targetReg, 0, 0, data[1]);

		result.push_back(targetReg);
	}
	else if(constant->type == VmType::Long)
	{
		unsigned data[2];
		memcpy(data, &constant->lValue, 8);

		unsigned char targetReg = lowFunction->GetRegisterForConstant();

		lowBlock->AddInstruction(ctx, constant->source, rviLoadImm, targetReg, 0, 0, data[0]);
		lowBlock->AddInstruction(ctx, constant->source, rviLoadImmLong, targetReg, 0, 0, data[1]);

		result.push_back(targetReg);
	}
	else if(constant->type.type == VM_TYPE_POINTER)
	{
		unsigned char targetReg = lowFunction->GetRegisterForConstant();

		if(!constant->container)
		{
			assert(constant->iValue == 0);

			if(NULLC_PTR_SIZE == 8)
			{
				lowBlock->AddInstruction(ctx, constant->source, rviLoadImm, targetReg, 0, 0, 0u);
				lowBlock->AddInstruction(ctx, constant->source, rviLoadImmLong, targetReg, 0, 0, 0u);
			}
			else
			{
				lowBlock->AddInstruction(ctx, constant->source, rviLoadImm, targetReg, 0, 0, 0u);
			}
		}
		else
		{
			lowBlock->AddInstruction(ctx, constant->source, rviGetAddr, targetReg, 0, IsLocalScope(constant->container->scope) ? rvrrFrame : rvrrGlobals, constant);
		}

		result.push_back(targetReg);
	}
	else if(constant->type.type == VM_TYPE_STRUCT)
	{
		assert(constant->type.size % 4 == 0);

		bool subPos = false;

		for(unsigned i = 0; i < constant->type.size / 4; i++)
		{
			if(subPos)
			{
				unsigned char targetReg = result.back();

				lowBlock->AddInstruction(ctx, constant->source, rviLoadImmLong, targetReg, 0, 0, ((unsigned*)constant->sValue)[i]);
			}
			else
			{
				unsigned char targetReg = lowFunction->GetRegisterForConstant();
				result.push_back(targetReg);

				lowBlock->AddInstruction(ctx, constant->source, rviLoadImm, targetReg, 0, 0, ((unsigned*)constant->sValue)[i]);
			}

			subPos = !subPos;
		}
	}
	else
	{
		assert(!"unknown type");
	}
}

bool TryLowerConstantPushIntoBlock(ExpressionContext &ctx, RegVmLoweredBlock *lowBlock, VmValue *value)
{
	if(VmConstant *constant = getType<VmConstant>(value))
	{
		if(constant->type == VmType::Int)
		{
			lowBlock->AddInstruction(ctx, constant->source, rviPushImm, 0, 0, 0, constant->iValue);

			return true;
		}
		else if(constant->type == VmType::Double)
		{
			unsigned data[2];
			memcpy(data, &constant->dValue, 8);

			lowBlock->AddInstruction(ctx, constant->source, rviPushImm, 0, 0, 0, data[0]);
			lowBlock->AddInstruction(ctx, constant->source, rviPushImm, 0, 0, 0, data[1]);

			return true;
		}
		else if(constant->type == VmType::Long)
		{
			unsigned data[2];
			memcpy(data, &constant->lValue, 8);

			if(data[1] == 0)
			{
				lowBlock->AddInstruction(ctx, constant->source, rviPushImmq, 0, 0, 0, data[0]);
			}
			else
			{
				lowBlock->AddInstruction(ctx, constant->source, rviPushImm, 0, 0, 0, data[0]);
				lowBlock->AddInstruction(ctx, constant->source, rviPushImm, 0, 0, 0, data[1]);
			}

			return true;
		}
		else if(constant->type.type == VM_TYPE_POINTER)
		{
			if(!constant->container)
			{
				assert(constant->iValue == 0);

				if(NULLC_PTR_SIZE == 8)
					lowBlock->AddInstruction(ctx, constant->source, rviPushImmq, 0, 0, 0, 0u);
				else
					lowBlock->AddInstruction(ctx, constant->source, rviPushImm, 0, 0, 0, 0u);

				return true;
			}
		}
	}

	return false;
}

unsigned char GetArgumentRegister(ExpressionContext &ctx, RegVmLoweredFunction *lowFunction, RegVmLoweredBlock *lowBlock, VmValue *value)
{
	if(isType<VmConstant>(value))
	{
		SmallArray<unsigned char, 32> result(ctx.allocator);

		LowerConstantIntoBlock(ctx, lowFunction, lowBlock, result, value);

		assert(result.size() == 1);

		return result.back();
	}

	if(VmFunction *function = getType<VmFunction>(value))
	{
		unsigned char targetReg = lowFunction->GetRegisterForConstant();

		lowBlock->AddInstruction(ctx, function->source, rviFuncAddr, targetReg, 0, 0, function->function->functionIndex);

		return targetReg;
	}

	return lowFunction->GetRegister(value);
}

void GetArgumentRegisters(ExpressionContext &ctx, RegVmLoweredFunction *lowFunction, RegVmLoweredBlock *lowBlock, SmallArray<unsigned char, 32> &result, VmValue *value)
{
	if(isType<VmConstant>(value))
	{
		LowerConstantIntoBlock(ctx, lowFunction, lowBlock, result, value);

		return;
	}

	if(VmFunction *function = getType<VmFunction>(value))
	{
		unsigned char targetReg = lowFunction->GetRegisterForConstant();

		lowBlock->AddInstruction(ctx, function->source, rviFuncAddr, targetReg, 0, 0, function->function->functionIndex);

		result.push_back(targetReg);

		return;
	}

	lowFunction->GetRegisters(result, value);
}

void LowerInstructionIntoBlock(ExpressionContext &ctx, RegVmLoweredFunction *lowFunction, RegVmLoweredBlock *lowBlock, VmValue *value)
{
	RegVmLoweredInstruction *lastLowered = lowBlock->lastInstruction;

	VmInstruction *inst = getType<VmInstruction>(value);

	assert(inst);

	switch(inst->cmd)
	{
	case VM_INST_LOAD_BYTE:
		if(VmConstant *constant = getType<VmConstant>(inst->arguments[0]))
		{
			VmConstant *offset = getType<VmConstant>(inst->arguments[1]);

			(void)offset;
			assert(offset->iValue == 0);

			unsigned char sourceReg = IsLocalScope(constant->container->scope) ? rvrrFrame : rvrrGlobals;
			unsigned char targetReg = lowFunction->AllocateRegister(inst);

			lowBlock->AddInstruction(ctx, inst->source, rviLoadByte, targetReg, 0, sourceReg, constant);
		}
		else
		{
			VmConstant *offset = getType<VmConstant>(inst->arguments[1]);

			unsigned char sourceReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
			unsigned char targetReg = lowFunction->AllocateRegister(inst);

			lowBlock->AddInstruction(ctx, inst->source, rviLoadByte, targetReg, 0, sourceReg, offset->iValue);
		}
		break;
	case VM_INST_LOAD_SHORT:
		if(VmConstant *constant = getType<VmConstant>(inst->arguments[0]))
		{
			VmConstant *offset = getType<VmConstant>(inst->arguments[1]);

			(void)offset;
			assert(offset->iValue == 0);

			unsigned char sourceReg = IsLocalScope(constant->container->scope) ? rvrrFrame : rvrrGlobals;
			unsigned char targetReg = lowFunction->AllocateRegister(inst);

			lowBlock->AddInstruction(ctx, inst->source, rviLoadWord, targetReg, 0, sourceReg, constant);
		}
		else
		{
			VmConstant *offset = getType<VmConstant>(inst->arguments[1]);

			unsigned char sourceReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
			unsigned char targetReg = lowFunction->AllocateRegister(inst);

			lowBlock->AddInstruction(ctx, inst->source, rviLoadWord, targetReg, 0, sourceReg, offset->iValue);
		}
		break;
	case VM_INST_LOAD_INT:
		if(VmConstant *constant = getType<VmConstant>(inst->arguments[0]))
		{
			VmConstant *offset = getType<VmConstant>(inst->arguments[1]);

			(void)offset;
			assert(offset->iValue == 0);

			unsigned char sourceReg = IsLocalScope(constant->container->scope) ? rvrrFrame : rvrrGlobals;
			unsigned char targetReg = lowFunction->AllocateRegister(inst);

			lowBlock->AddInstruction(ctx, inst->source, rviLoadDword, targetReg, 0, sourceReg, constant);
		}
		else
		{
			VmConstant *offset = getType<VmConstant>(inst->arguments[1]);

			unsigned char sourceReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
			unsigned char targetReg = lowFunction->AllocateRegister(inst);

			lowBlock->AddInstruction(ctx, inst->source, rviLoadDword, targetReg, 0, sourceReg, offset->iValue);
		}
		break;
	case VM_INST_LOAD_FLOAT:
		if(VmConstant *constant = getType<VmConstant>(inst->arguments[0]))
		{
			VmConstant *offset = getType<VmConstant>(inst->arguments[1]);

			(void)offset;
			assert(offset->iValue == 0);

			unsigned char sourceReg = IsLocalScope(constant->container->scope) ? rvrrFrame : rvrrGlobals;
			unsigned char targetReg = lowFunction->AllocateRegister(inst);

			lowBlock->AddInstruction(ctx, inst->source, rviLoadFloat, targetReg, 0, sourceReg, constant);
		}
		else
		{
			VmConstant *offset = getType<VmConstant>(inst->arguments[1]);

			unsigned char sourceReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
			unsigned char targetReg = lowFunction->AllocateRegister(inst);

			lowBlock->AddInstruction(ctx, inst->source, rviLoadFloat, targetReg, 0, sourceReg, offset->iValue);
		}
		break;
	case VM_INST_LOAD_DOUBLE:
		if(VmConstant *constant = getType<VmConstant>(inst->arguments[0]))
		{
			VmConstant *offset = getType<VmConstant>(inst->arguments[1]);

			(void)offset;
			assert(offset->iValue == 0);

			unsigned char sourceReg = IsLocalScope(constant->container->scope) ? rvrrFrame : rvrrGlobals;
			unsigned char targetReg = lowFunction->AllocateRegister(inst);

			lowBlock->AddInstruction(ctx, inst->source, rviLoadDouble, targetReg, 0, sourceReg, constant);
		}
		else
		{
			VmConstant *offset = getType<VmConstant>(inst->arguments[1]);

			unsigned char sourceReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
			unsigned char targetReg = lowFunction->AllocateRegister(inst);

			lowBlock->AddInstruction(ctx, inst->source, rviLoadDouble, targetReg, 0, sourceReg, offset->iValue);
		}
		break;
	case VM_INST_LOAD_LONG:
		if(VmConstant *constant = getType<VmConstant>(inst->arguments[0]))
		{
			VmConstant *offset = getType<VmConstant>(inst->arguments[1]);

			(void)offset;
			assert(offset->iValue == 0);

			unsigned char sourceReg = IsLocalScope(constant->container->scope) ? rvrrFrame : rvrrGlobals;
			unsigned char targetReg = lowFunction->AllocateRegister(inst);

			lowBlock->AddInstruction(ctx, inst->source, rviLoadLong, targetReg, 0, sourceReg, constant);
		}
		else
		{
			VmConstant *offset = getType<VmConstant>(inst->arguments[1]);

			unsigned char sourceReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
			unsigned char targetReg = lowFunction->AllocateRegister(inst);

			lowBlock->AddInstruction(ctx, inst->source, rviLoadLong, targetReg, 0, sourceReg, offset->iValue);
		}
		break;
	case VM_INST_LOAD_STRUCT:
		if(VmConstant *constant = getType<VmConstant>(inst->arguments[0]))
		{
			VmConstant *offset = getType<VmConstant>(inst->arguments[1]);

			(void)offset;
			assert(offset->iValue == 0);

			assert((unsigned short)inst->type.size == inst->type.size);

			unsigned char addressReg = IsLocalScope(constant->container->scope) ? rvrrFrame : rvrrGlobals;

			unsigned pos = 0;

			if(inst->type.type == VM_TYPE_FUNCTION_REF || inst->type.type == VM_TYPE_ARRAY_REF)
			{
				if(NULLC_PTR_SIZE == 8)
				{
					lowBlock->AddInstruction(ctx, inst->source, rviLoadLong, lowFunction->AllocateRegister(inst, 0, false), 0, addressReg, CreateConstantPointer(ctx.allocator, constant->source, constant->iValue + pos, constant->container, constant->type.structType, false));
					pos += 8;

					lowBlock->AddInstruction(ctx, inst->source, rviLoadDword, lowFunction->AllocateRegister(inst, 1, true), 0, addressReg, CreateConstantPointer(ctx.allocator, constant->source, constant->iValue + pos, constant->container, constant->type.structType, false));
					pos += 4;
				}
				else
				{
					lowBlock->AddInstruction(ctx, inst->source, rviLoadDword, lowFunction->AllocateRegister(inst, 0, false), 0, addressReg, CreateConstantPointer(ctx.allocator, constant->source, constant->iValue + pos, constant->container, constant->type.structType, false));
					pos += 4;

					lowBlock->AddInstruction(ctx, inst->source, rviLoadDword, lowFunction->AllocateRegister(inst, 1, true), 0, addressReg, CreateConstantPointer(ctx.allocator, constant->source, constant->iValue + pos, constant->container, constant->type.structType, false));
					pos += 4;
				}
			}
			else if(inst->type.type == VM_TYPE_AUTO_REF)
			{
				if(NULLC_PTR_SIZE == 8)
				{
					lowBlock->AddInstruction(ctx, inst->source, rviLoadDword, lowFunction->AllocateRegister(inst, 0, false), 0, addressReg, CreateConstantPointer(ctx.allocator, constant->source, constant->iValue + pos, constant->container, constant->type.structType, false));
					pos += 4;

					lowBlock->AddInstruction(ctx, inst->source, rviLoadLong, lowFunction->AllocateRegister(inst, 1, true), 0, addressReg, CreateConstantPointer(ctx.allocator, constant->source, constant->iValue + pos, constant->container, constant->type.structType, false));
					pos += 8;
				}
				else
				{
					lowBlock->AddInstruction(ctx, inst->source, rviLoadDword, lowFunction->AllocateRegister(inst, 0, false), 0, addressReg, CreateConstantPointer(ctx.allocator, constant->source, constant->iValue + pos, constant->container, constant->type.structType, false));
					pos += 4;

					lowBlock->AddInstruction(ctx, inst->source, rviLoadDword, lowFunction->AllocateRegister(inst, 1, true), 0, addressReg, CreateConstantPointer(ctx.allocator, constant->source, constant->iValue + pos, constant->container, constant->type.structType, false));
					pos += 4;
				}
			}
			else if(inst->type.type == VM_TYPE_AUTO_ARRAY)
			{
				if(NULLC_PTR_SIZE == 8)
				{
					lowBlock->AddInstruction(ctx, inst->source, rviLoadDword, lowFunction->AllocateRegister(inst, 0, false), 0, addressReg, CreateConstantPointer(ctx.allocator, constant->source, constant->iValue + pos, constant->container, constant->type.structType, false));
					pos += 4;

					lowBlock->AddInstruction(ctx, inst->source, rviLoadLong, lowFunction->AllocateRegister(inst, 1, false), 0, addressReg, CreateConstantPointer(ctx.allocator, constant->source, constant->iValue + pos, constant->container, constant->type.structType, false));
					pos += 8;

					lowBlock->AddInstruction(ctx, inst->source, rviLoadDword, lowFunction->AllocateRegister(inst, 2, true), 0, addressReg, CreateConstantPointer(ctx.allocator, constant->source, constant->iValue + pos, constant->container, constant->type.structType, false));
					pos += 4;
				}
				else
				{
					lowBlock->AddInstruction(ctx, inst->source, rviLoadDword, lowFunction->AllocateRegister(inst, 0, false), 0, addressReg, CreateConstantPointer(ctx.allocator, constant->source, constant->iValue + pos, constant->container, constant->type.structType, false));
					pos += 4;

					lowBlock->AddInstruction(ctx, inst->source, rviLoadDword, lowFunction->AllocateRegister(inst, 1, false), 0, addressReg, CreateConstantPointer(ctx.allocator, constant->source, constant->iValue + pos, constant->container, constant->type.structType, false));
					pos += 4;

					lowBlock->AddInstruction(ctx, inst->source, rviLoadDword, lowFunction->AllocateRegister(inst, 2, true), 0, addressReg, CreateConstantPointer(ctx.allocator, constant->source, constant->iValue + pos, constant->container, constant->type.structType, false));
					pos += 4;
				}
			}
			else if(inst->type.type == VM_TYPE_STRUCT)
			{
				unsigned index = 0;

				unsigned remainingSize = inst->type.size;

				assert(remainingSize % 4 == 0);

				while(remainingSize != 0)
				{
					if(remainingSize == 4)
					{
						lowBlock->AddInstruction(ctx, inst->source, rviLoadDword, lowFunction->AllocateRegister(inst, index++, true), 0, addressReg, CreateConstantPointer(ctx.allocator, constant->source, constant->iValue + pos, constant->container, constant->type.structType, false));
						pos += 4;

						remainingSize -= 4;
					}
					else
					{
						lowBlock->AddInstruction(ctx, inst->source, rviLoadLong, lowFunction->AllocateRegister(inst, index++, remainingSize == 8), 0, addressReg, CreateConstantPointer(ctx.allocator, constant->source, constant->iValue + pos, constant->container, constant->type.structType, false));
						pos += 8;

						remainingSize -= 8;
					}
				}
			}
		}
		else
		{
			VmConstant *offset = getType<VmConstant>(inst->arguments[1]);

			unsigned char addressReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);

			assert((unsigned short)inst->type.size == inst->type.size);

			unsigned pos = offset->iValue;

			if(inst->type.type == VM_TYPE_FUNCTION_REF || inst->type.type == VM_TYPE_ARRAY_REF)
			{
				if(NULLC_PTR_SIZE == 8)
				{
					lowBlock->AddInstruction(ctx, inst->source, rviLoadLong, lowFunction->AllocateRegister(inst, 0, false), 0, addressReg, pos);
					pos += 8;

					lowBlock->AddInstruction(ctx, inst->source, rviLoadDword, lowFunction->AllocateRegister(inst, 1, true), 0, addressReg, pos);
					pos += 4;
				}
				else
				{
					lowBlock->AddInstruction(ctx, inst->source, rviLoadDword, lowFunction->AllocateRegister(inst, 0, false), 0, addressReg, pos);
					pos += 4;

					lowBlock->AddInstruction(ctx, inst->source, rviLoadDword, lowFunction->AllocateRegister(inst, 1, true), 0, addressReg, pos);
					pos += 4;
				}
			}
			else if(inst->type.type == VM_TYPE_AUTO_REF)
			{
				if(NULLC_PTR_SIZE == 8)
				{
					lowBlock->AddInstruction(ctx, inst->source, rviLoadDword, lowFunction->AllocateRegister(inst, 0, false), 0, addressReg, pos);
					pos += 4;

					lowBlock->AddInstruction(ctx, inst->source, rviLoadLong, lowFunction->AllocateRegister(inst, 1, true), 0, addressReg, pos);
					pos += 8;
				}
				else
				{
					lowBlock->AddInstruction(ctx, inst->source, rviLoadDword, lowFunction->AllocateRegister(inst, 0, false), 0, addressReg, pos);
					pos += 4;

					lowBlock->AddInstruction(ctx, inst->source, rviLoadDword, lowFunction->AllocateRegister(inst, 1, true), 0, addressReg, pos);
					pos += 4;
				}
			}
			else if(inst->type.type == VM_TYPE_AUTO_ARRAY)
			{
				if(NULLC_PTR_SIZE == 8)
				{
					lowBlock->AddInstruction(ctx, inst->source, rviLoadDword, lowFunction->AllocateRegister(inst, 0, false), 0, addressReg, pos);
					pos += 4;

					lowBlock->AddInstruction(ctx, inst->source, rviLoadLong, lowFunction->AllocateRegister(inst, 1, false), 0, addressReg, pos);
					pos += 8;

					lowBlock->AddInstruction(ctx, inst->source, rviLoadDword, lowFunction->AllocateRegister(inst, 2, true), 0, addressReg, pos);
					pos += 4;
				}
				else
				{
					lowBlock->AddInstruction(ctx, inst->source, rviLoadDword, lowFunction->AllocateRegister(inst, 0, false), 0, addressReg, pos);
					pos += 4;

					lowBlock->AddInstruction(ctx, inst->source, rviLoadDword, lowFunction->AllocateRegister(inst, 1, false), 0, addressReg, pos);
					pos += 4;

					lowBlock->AddInstruction(ctx, inst->source, rviLoadDword, lowFunction->AllocateRegister(inst, 2, true), 0, addressReg, pos);
					pos += 4;
				}
			}
			else if(inst->type.type == VM_TYPE_STRUCT)
			{
				unsigned index = 0;

				unsigned remainingSize = inst->type.size;

				assert(remainingSize % 4 == 0);

				while(remainingSize != 0)
				{
					if(remainingSize == 4)
					{
						lowBlock->AddInstruction(ctx, inst->source, rviLoadDword, lowFunction->AllocateRegister(inst, index++, true), 0, addressReg, pos);
						pos += 4;

						remainingSize -= 4;
					}
					else
					{
						lowBlock->AddInstruction(ctx, inst->source, rviLoadLong, lowFunction->AllocateRegister(inst, index++, remainingSize == 8), 0, addressReg, pos);
						pos += 8;

						remainingSize -= 8;
					}
				}
			}
		}
		break;
	case VM_INST_LOAD_IMMEDIATE:
		if(VmConstant *constant = getType<VmConstant>(inst->arguments[0]))
		{
			unsigned char targetReg = lowFunction->AllocateRegister(inst);

			// Handle all integers but other types only with a value of 0
			if(inst->arguments[0]->type == VmType::Int)
			{
				lowBlock->AddInstruction(ctx, inst->source, rviLoadImm, targetReg, 0, 0, constant);
			}
			else if(inst->arguments[0]->type == VmType::Double)
			{
				unsigned data[2];
				memcpy(data, &constant->dValue, 8);

				lowBlock->AddInstruction(ctx, inst->source, rviLoadImm, targetReg, 0, 0, data[0]);
				lowBlock->AddInstruction(ctx, inst->source, rviLoadImmDouble, targetReg, 0, 0, data[1]);
			}
			else if(inst->arguments[0]->type == VmType::Long)
			{
				unsigned data[2];
				memcpy(data, &constant->lValue, 8);

				lowBlock->AddInstruction(ctx, inst->source, rviLoadImm, targetReg, 0, 0, data[0]);
				lowBlock->AddInstruction(ctx, inst->source, rviLoadImmLong, targetReg, 0, 0, data[1]);
			}
			else if(inst->arguments[0]->type.type == VM_TYPE_POINTER)
			{
				if(constant->container)
				{
					lowBlock->AddInstruction(ctx, inst->source, rviGetAddr, targetReg, 0, IsLocalScope(constant->container->scope) ? rvrrFrame : rvrrGlobals, constant);
				}
				else
				{
					assert(!constant->container);

					lowBlock->AddInstruction(ctx, inst->source, rviLoadImm, targetReg, 0, 0, 0u);

					if(NULLC_PTR_SIZE == 8)
						lowBlock->AddInstruction(ctx, inst->source, rviLoadImmLong, targetReg, 0, 0, 0u);
				}
			}
			else
			{
				assert(!"unknown type");
			}
		}
		break;
	case VM_INST_STORE_BYTE:
		if(VmConstant *constant = getType<VmConstant>(inst->arguments[0]))
		{
			VmConstant *offset = getType<VmConstant>(inst->arguments[1]);

			(void)offset;
			assert(offset->iValue == 0);

			unsigned char sourceReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[2]);
			unsigned char addressReg = IsLocalScope(constant->container->scope) ? rvrrFrame : rvrrGlobals;

			lowBlock->AddInstruction(ctx, inst->source, rviStoreByte, sourceReg, 0, addressReg, constant);
		}
		else
		{
			VmConstant *offset = getType<VmConstant>(inst->arguments[1]);

			unsigned char sourceReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[2]);
			unsigned char addressReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);

			lowBlock->AddInstruction(ctx, inst->source, rviStoreByte, sourceReg, 0, addressReg, offset);
		}
		break;
	case VM_INST_STORE_SHORT:
		if(VmConstant *constant = getType<VmConstant>(inst->arguments[0]))
		{
			VmConstant *offset = getType<VmConstant>(inst->arguments[1]);

			(void)offset;
			assert(offset->iValue == 0);

			unsigned char sourceReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[2]);
			unsigned char addressReg = IsLocalScope(constant->container->scope) ? rvrrFrame : rvrrGlobals;

			lowBlock->AddInstruction(ctx, inst->source, rviStoreWord, sourceReg, 0, addressReg, constant);
		}
		else
		{
			VmConstant *offset = getType<VmConstant>(inst->arguments[1]);

			unsigned char sourceReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[2]);
			unsigned char addressReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);

			lowBlock->AddInstruction(ctx, inst->source, rviStoreWord, sourceReg, 0, addressReg, offset);
		}
		break;
	case VM_INST_STORE_INT:
		if(VmConstant *constant = getType<VmConstant>(inst->arguments[0]))
		{
			VmConstant *offset = getType<VmConstant>(inst->arguments[1]);

			(void)offset;
			assert(offset->iValue == 0);

			unsigned char sourceReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[2]);
			unsigned char addressReg = IsLocalScope(constant->container->scope) ? rvrrFrame : rvrrGlobals;

			lowBlock->AddInstruction(ctx, inst->source, rviStoreDword, sourceReg, 0, addressReg, constant);
		}
		else
		{
			VmConstant *offset = getType<VmConstant>(inst->arguments[1]);

			unsigned char sourceReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[2]);
			unsigned char addressReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);

			lowBlock->AddInstruction(ctx, inst->source, rviStoreDword, sourceReg, 0, addressReg, offset);
		}
		break;
	case VM_INST_STORE_FLOAT:
		if(VmConstant *constant = getType<VmConstant>(inst->arguments[0]))
		{
			VmConstant *offset = getType<VmConstant>(inst->arguments[1]);

			(void)offset;
			assert(offset->iValue == 0);

			unsigned char sourceReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[2]);
			unsigned char addressReg = IsLocalScope(constant->container->scope) ? rvrrFrame : rvrrGlobals;

			lowBlock->AddInstruction(ctx, inst->source, rviStoreFloat, sourceReg, 0, addressReg, constant);
		}
		else
		{
			VmConstant *offset = getType<VmConstant>(inst->arguments[1]);

			unsigned char sourceReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[2]);
			unsigned char addressReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);

			lowBlock->AddInstruction(ctx, inst->source, rviStoreFloat, sourceReg, 0, addressReg, offset);
		}
		break;
	case VM_INST_STORE_LONG:
		if(VmConstant *constant = getType<VmConstant>(inst->arguments[0]))
		{
			VmConstant *offset = getType<VmConstant>(inst->arguments[1]);

			(void)offset;
			assert(offset->iValue == 0);

			unsigned char sourceReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[2]);
			unsigned char addressReg = IsLocalScope(constant->container->scope) ? rvrrFrame : rvrrGlobals;

			lowBlock->AddInstruction(ctx, inst->source, rviStoreLong, sourceReg, 0, addressReg, constant);
		}
		else
		{
			VmConstant *offset = getType<VmConstant>(inst->arguments[1]);

			unsigned char sourceReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[2]);
			unsigned char addressReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);

			lowBlock->AddInstruction(ctx, inst->source, rviStoreLong, sourceReg, 0, addressReg, offset);
		}
		break;
	case VM_INST_STORE_DOUBLE:
		if(VmConstant *constant = getType<VmConstant>(inst->arguments[0]))
		{
			VmConstant *offset = getType<VmConstant>(inst->arguments[1]);

			(void)offset;
			assert(offset->iValue == 0);

			unsigned char sourceReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[2]);
			unsigned char addressReg = IsLocalScope(constant->container->scope) ? rvrrFrame : rvrrGlobals;

			lowBlock->AddInstruction(ctx, inst->source, rviStoreDouble, sourceReg, 0, addressReg, constant);
		}
		else
		{
			VmConstant *offset = getType<VmConstant>(inst->arguments[1]);

			unsigned char sourceReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[2]);
			unsigned char addressReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);

			lowBlock->AddInstruction(ctx, inst->source, rviStoreDouble, sourceReg, 0, addressReg, offset);
		}
		break;
	case VM_INST_STORE_STRUCT:
		if(VmConstant *constant = getType<VmConstant>(inst->arguments[0]))
		{
			VmConstant *offset = getType<VmConstant>(inst->arguments[1]);

			(void)offset;
			assert(offset->iValue == 0);

			SmallArray<unsigned char, 32> sourceRegs(ctx.allocator);
			GetArgumentRegisters(ctx, lowFunction, lowBlock, sourceRegs, inst->arguments[2]);

			assert((unsigned short)inst->arguments[2]->type.size == inst->arguments[2]->type.size);

			unsigned char addressReg = IsLocalScope(constant->container->scope) ? rvrrFrame : rvrrGlobals;

			unsigned pos = 0;

			if(inst->arguments[2]->type.type == VM_TYPE_FUNCTION_REF || inst->arguments[2]->type.type == VM_TYPE_ARRAY_REF)
			{
				if(NULLC_PTR_SIZE == 8)
				{
					lowBlock->AddInstruction(ctx, inst->source, rviStoreLong, sourceRegs[0], 0, addressReg, CreateConstantPointer(ctx.allocator, constant->source, constant->iValue + pos, constant->container, constant->type.structType, false));
					pos += 8;

					lowBlock->AddInstruction(ctx, inst->source, rviStoreDword, sourceRegs[1], 0, addressReg, CreateConstantPointer(ctx.allocator, constant->source, constant->iValue + pos, constant->container, constant->type.structType, false));
					pos += 4;
				}
				else
				{
					lowBlock->AddInstruction(ctx, inst->source, rviStoreDword, sourceRegs[0], 0, addressReg, CreateConstantPointer(ctx.allocator, constant->source, constant->iValue + pos, constant->container, constant->type.structType, false));
					pos += 4;

					lowBlock->AddInstruction(ctx, inst->source, rviStoreDword, sourceRegs[1], 0, addressReg, CreateConstantPointer(ctx.allocator, constant->source, constant->iValue + pos, constant->container, constant->type.structType, false));
					pos += 4;
				}
			}
			else if(inst->arguments[2]->type.type == VM_TYPE_AUTO_REF)
			{
				if(NULLC_PTR_SIZE == 8)
				{
					lowBlock->AddInstruction(ctx, inst->source, rviStoreDword, sourceRegs[0], 0, addressReg, CreateConstantPointer(ctx.allocator, constant->source, constant->iValue + pos, constant->container, constant->type.structType, false));
					pos += 4;

					lowBlock->AddInstruction(ctx, inst->source, rviStoreLong, sourceRegs[1], 0, addressReg, CreateConstantPointer(ctx.allocator, constant->source, constant->iValue + pos, constant->container, constant->type.structType, false));
					pos += 8;
				}
				else
				{
					lowBlock->AddInstruction(ctx, inst->source, rviStoreDword, sourceRegs[0], 0, addressReg, CreateConstantPointer(ctx.allocator, constant->source, constant->iValue + pos, constant->container, constant->type.structType, false));
					pos += 4;

					lowBlock->AddInstruction(ctx, inst->source, rviStoreDword, sourceRegs[1], 0, addressReg, CreateConstantPointer(ctx.allocator, constant->source, constant->iValue + pos, constant->container, constant->type.structType, false));
					pos += 4;
				}
			}
			else if(inst->arguments[2]->type.type == VM_TYPE_AUTO_ARRAY)
			{
				if(NULLC_PTR_SIZE == 8)
				{
					lowBlock->AddInstruction(ctx, inst->source, rviStoreDword, sourceRegs[0], 0, addressReg, CreateConstantPointer(ctx.allocator, constant->source, constant->iValue + pos, constant->container, constant->type.structType, false));
					pos += 4;

					lowBlock->AddInstruction(ctx, inst->source, rviStoreLong, sourceRegs[1], 0, addressReg, CreateConstantPointer(ctx.allocator, constant->source, constant->iValue + pos, constant->container, constant->type.structType, false));
					pos += 8;

					lowBlock->AddInstruction(ctx, inst->source, rviStoreDword, sourceRegs[2], 0, addressReg, CreateConstantPointer(ctx.allocator, constant->source, constant->iValue + pos, constant->container, constant->type.structType, false));
					pos += 4;
				}
				else
				{
					lowBlock->AddInstruction(ctx, inst->source, rviStoreDword, sourceRegs[0], 0, addressReg, CreateConstantPointer(ctx.allocator, constant->source, constant->iValue + pos, constant->container, constant->type.structType, false));
					pos += 4;

					lowBlock->AddInstruction(ctx, inst->source, rviStoreDword, sourceRegs[1], 0, addressReg, CreateConstantPointer(ctx.allocator, constant->source, constant->iValue + pos, constant->container, constant->type.structType, false));
					pos += 4;

					lowBlock->AddInstruction(ctx, inst->source, rviStoreDword, sourceRegs[2], 0, addressReg, CreateConstantPointer(ctx.allocator, constant->source, constant->iValue + pos, constant->container, constant->type.structType, false));
					pos += 4;
				}
			}
			else if(inst->arguments[2]->type.type == VM_TYPE_STRUCT)
			{
				unsigned remainingSize = inst->arguments[2]->type.size;

				assert(remainingSize % 4 == 0);

				for(unsigned i = 0; i < sourceRegs.size(); i++)
				{
					if(remainingSize == 4)
					{
						lowBlock->AddInstruction(ctx, inst->source, rviStoreDword, sourceRegs[i], 0, addressReg, CreateConstantPointer(ctx.allocator, constant->source, constant->iValue + pos, constant->container, constant->type.structType, false));
						pos += 4;

						remainingSize -= 4;
					}
					else
					{
						lowBlock->AddInstruction(ctx, inst->source, rviStoreLong, sourceRegs[i], 0, addressReg, CreateConstantPointer(ctx.allocator, constant->source, constant->iValue + pos, constant->container, constant->type.structType, false));
						pos += 8;

						remainingSize -= 8;
					}
				}
			}
		}
		else
		{
			VmConstant *offset = getType<VmConstant>(inst->arguments[1]);

			SmallArray<unsigned char, 32> sourceRegs(ctx.allocator);
			GetArgumentRegisters(ctx, lowFunction, lowBlock, sourceRegs, inst->arguments[2]);

			unsigned char addressReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);

			assert((unsigned short)inst->arguments[2]->type.size == inst->arguments[2]->type.size);

			unsigned pos = offset->iValue;

			if(inst->arguments[2]->type.type == VM_TYPE_FUNCTION_REF || inst->arguments[2]->type.type == VM_TYPE_ARRAY_REF)
			{
				if(NULLC_PTR_SIZE == 8)
				{
					lowBlock->AddInstruction(ctx, inst->source, rviStoreLong, sourceRegs[0], 0, addressReg, pos);
					pos += 8;

					lowBlock->AddInstruction(ctx, inst->source, rviStoreDword, sourceRegs[1], 0, addressReg, pos);
					pos += 4;
				}
				else
				{
					lowBlock->AddInstruction(ctx, inst->source, rviStoreDword, sourceRegs[0], 0, addressReg, pos);
					pos += 4;

					lowBlock->AddInstruction(ctx, inst->source, rviStoreDword, sourceRegs[1], 0, addressReg, pos);
					pos += 4;
				}
			}
			else if(inst->arguments[2]->type.type == VM_TYPE_AUTO_REF)
			{
				if(NULLC_PTR_SIZE == 8)
				{
					lowBlock->AddInstruction(ctx, inst->source, rviStoreDword, sourceRegs[0], 0, addressReg, pos);
					pos += 4;

					lowBlock->AddInstruction(ctx, inst->source, rviStoreLong, sourceRegs[1], 0, addressReg, pos);
					pos += 8;
				}
				else
				{
					lowBlock->AddInstruction(ctx, inst->source, rviStoreDword, sourceRegs[0], 0, addressReg, pos);
					pos += 4;

					lowBlock->AddInstruction(ctx, inst->source, rviStoreDword, sourceRegs[1], 0, addressReg, pos);
					pos += 4;
				}
			}
			else if(inst->arguments[2]->type.type == VM_TYPE_AUTO_ARRAY)
			{
				if(NULLC_PTR_SIZE == 8)
				{
					lowBlock->AddInstruction(ctx, inst->source, rviStoreDword, sourceRegs[0], 0, addressReg, pos);
					pos += 4;

					lowBlock->AddInstruction(ctx, inst->source, rviStoreLong, sourceRegs[1], 0, addressReg, pos);
					pos += 8;

					lowBlock->AddInstruction(ctx, inst->source, rviStoreDword, sourceRegs[2], 0, addressReg, pos);
					pos += 4;
				}
				else
				{
					lowBlock->AddInstruction(ctx, inst->source, rviStoreDword, sourceRegs[0], 0, addressReg, pos);
					pos += 4;

					lowBlock->AddInstruction(ctx, inst->source, rviStoreDword, sourceRegs[1], 0, addressReg, pos);
					pos += 4;

					lowBlock->AddInstruction(ctx, inst->source, rviStoreDword, sourceRegs[2], 0, addressReg, pos);
					pos += 4;
				}
			}
			else if(inst->arguments[2]->type.type == VM_TYPE_STRUCT)
			{
				unsigned remainingSize = inst->arguments[2]->type.size;

				assert(remainingSize % 4 == 0);

				for(unsigned i = 0; i < sourceRegs.size(); i++)
				{
					if(remainingSize == 4)
					{
						lowBlock->AddInstruction(ctx, inst->source, rviStoreDword, sourceRegs[i], 0, addressReg, pos);
						pos += 4;

						remainingSize -= 4;
					}
					else
					{
						lowBlock->AddInstruction(ctx, inst->source, rviStoreLong, sourceRegs[i], 0, addressReg, pos);
						pos += 8;

						remainingSize -= 8;
					}
				}
			}
		}
		break;
	case VM_INST_DOUBLE_TO_INT:
	{
		unsigned char sourceReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
		unsigned char targetReg = lowFunction->AllocateRegister(inst);

		lowBlock->AddInstruction(ctx, inst->source, rviDtoi, targetReg, 0, sourceReg);
	}
		break;
	case VM_INST_DOUBLE_TO_LONG:
	{
		unsigned char sourceReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
		unsigned char targetReg = lowFunction->AllocateRegister(inst);

		lowBlock->AddInstruction(ctx, inst->source, rviDtol, targetReg, 0, sourceReg);
	}
		break;
	case VM_INST_DOUBLE_TO_FLOAT:
		if(VmConstant *argument = getType<VmConstant>(inst->arguments[0]))
		{
			float result = float(argument->dValue);

			unsigned target = 0;
			memcpy(&target, &result, sizeof(float));

			unsigned char targetReg = lowFunction->AllocateRegister(inst);

			lowBlock->AddInstruction(ctx, inst->source, rviLoadImm, targetReg, 0, 0, target);
		}
		else
		{
			unsigned char sourceReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
			unsigned char targetReg = lowFunction->AllocateRegister(inst);

			lowBlock->AddInstruction(ctx, inst->source, rviDtof, targetReg, 0, sourceReg);
		}
		break;
	case VM_INST_INT_TO_DOUBLE:
	{
		unsigned char sourceReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
		unsigned char targetReg = lowFunction->AllocateRegister(inst);

		lowBlock->AddInstruction(ctx, inst->source, rviItod, targetReg, 0, sourceReg);
	}
		break;
	case VM_INST_LONG_TO_DOUBLE:
	{
		unsigned char sourceReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
		unsigned char targetReg = lowFunction->AllocateRegister(inst);

		lowBlock->AddInstruction(ctx, inst->source, rviLtod, targetReg, 0, sourceReg);
	}
		break;
	case VM_INST_INT_TO_LONG:
	{
		unsigned char sourceReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
		unsigned char targetReg = lowFunction->AllocateRegister(inst);

		lowBlock->AddInstruction(ctx, inst->source, rviItol, targetReg, 0, sourceReg);
	}
		break;
	case VM_INST_LONG_TO_INT:
	{
		unsigned char sourceReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
		unsigned char targetReg = lowFunction->AllocateRegister(inst);

		lowBlock->AddInstruction(ctx, inst->source, rviLtoi, targetReg, 0, sourceReg);
	}
		break;
	case VM_INST_INDEX:
	{
		VmConstant *arrSize = getType<VmConstant>(inst->arguments[0]);
		VmConstant *elementSize = getType<VmConstant>(inst->arguments[1]);
		VmValue *pointer = inst->arguments[2];
		VmValue *index = inst->arguments[3];

		assert(arrSize && elementSize);

		unsigned char indexReg = GetArgumentRegister(ctx, lowFunction, lowBlock, index);
		unsigned char pointerReg = GetArgumentRegister(ctx, lowFunction, lowBlock, pointer);
		unsigned char arrSizeReg = GetArgumentRegister(ctx, lowFunction, lowBlock, arrSize);
		unsigned char targetReg = lowFunction->AllocateRegister(inst);

		assert((unsigned short)elementSize->iValue == elementSize->iValue);

		lowBlock->AddInstruction(ctx, inst->source, rviIndex, targetReg, indexReg, pointerReg, arrSizeReg << 16 | (unsigned short)elementSize->iValue);
	}
	break;
	case VM_INST_INDEX_UNSIZED:
	{
		VmConstant *elementSize = getType<VmConstant>(inst->arguments[0]);
		VmValue *arr = inst->arguments[1];
		VmValue *index = inst->arguments[2];

		assert(elementSize);

		unsigned char indexReg = GetArgumentRegister(ctx, lowFunction, lowBlock, index);

		SmallArray<unsigned char, 32> arrRegs(ctx.allocator);
		GetArgumentRegisters(ctx, lowFunction, lowBlock, arrRegs, arr);

		unsigned char targetReg = lowFunction->AllocateRegister(inst);

		assert((unsigned short)elementSize->iValue == elementSize->iValue);

		lowBlock->AddInstruction(ctx, inst->source, rviIndex, targetReg, indexReg, arrRegs[0], arrRegs[1] << 16 | (unsigned short)elementSize->iValue);
	}
	break;
	case VM_INST_FUNCTION_ADDRESS:
	{
		VmConstant *funcIndex = getType<VmConstant>(inst->arguments[0]);

		assert(funcIndex);

		unsigned char targetReg = lowFunction->AllocateRegister(inst);

		lowBlock->AddInstruction(ctx, inst->source, rviFuncAddr, targetReg, 0, 0, funcIndex);
	}
	break;
	case VM_INST_TYPE_ID:
	{
		VmConstant *typeIndex = getType<VmConstant>(inst->arguments[0]);

		assert(typeIndex);

		unsigned char targetReg = lowFunction->AllocateRegister(inst);

		lowBlock->AddInstruction(ctx, inst->source, rviTypeid, targetReg, 0, 0, typeIndex->iValue);
	}
	break;
	case VM_INST_SET_RANGE:
	{
		VmValue *address = inst->arguments[0];
		VmConstant *count = getType<VmConstant>(inst->arguments[1]);
		VmValue *initializer = inst->arguments[2];
		VmConstant *elementSize = getType<VmConstant>(inst->arguments[3]);

		unsigned char initializerReg = GetArgumentRegister(ctx, lowFunction, lowBlock, initializer);
		unsigned char addressReg = GetArgumentRegister(ctx, lowFunction, lowBlock, address);

		if(initializer->type == VmType::Int && elementSize->iValue == 1)
			lowBlock->AddInstruction(ctx, inst->source, rviSetRange, initializerReg, rvsrChar, addressReg, count->iValue);
		else if(initializer->type == VmType::Int && elementSize->iValue == 2)
			lowBlock->AddInstruction(ctx, inst->source, rviSetRange, initializerReg, rvsrShort, addressReg, count->iValue);
		else if(initializer->type == VmType::Int && elementSize->iValue == 4)
			lowBlock->AddInstruction(ctx, inst->source, rviSetRange, initializerReg, rvsrInt, addressReg, count->iValue);
		else if(initializer->type == VmType::Long)
			lowBlock->AddInstruction(ctx, inst->source, rviSetRange, initializerReg, rvsrLong, addressReg, count->iValue);
		else if(initializer->type == VmType::Double && elementSize->iValue == 4)
			lowBlock->AddInstruction(ctx, inst->source, rviSetRange, initializerReg, rvsrFloat, addressReg, count->iValue);
		else if(initializer->type == VmType::Double && elementSize->iValue == 8)
			lowBlock->AddInstruction(ctx, inst->source, rviSetRange, initializerReg, rvsrDouble, addressReg, count->iValue);
	}
		break;
	case VM_INST_JUMP:
		// Check if jump is fall-through
		if(!(lowBlock->vmBlock->nextSibling && lowBlock->vmBlock->nextSibling == inst->arguments[0]))
		{
			lowBlock->AddInstruction(ctx, inst->source, rviJmp, 0, 0, 0, getType<VmBlock>(inst->arguments[0]));
		}
		break;
	case VM_INST_JUMP_Z:
	{
		assert(inst->arguments[0]->type.size == 4);

		unsigned char sourceReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);

		// Check if one side of the jump is fall-through
		if(lowBlock->vmBlock->nextSibling && lowBlock->vmBlock->nextSibling == inst->arguments[1])
		{
			lowBlock->AddInstruction(ctx, inst->source, rviJmpnz, 0, 0, sourceReg, getType<VmBlock>(inst->arguments[2]));
		}
		else
		{
			lowBlock->AddInstruction(ctx, inst->source, rviJmpz, 0, 0, sourceReg, getType<VmBlock>(inst->arguments[1]));

			lowBlock->AddInstruction(ctx, inst->source, rviJmp, 0, 0, 0, getType<VmBlock>(inst->arguments[2]));
		}
	}
		break;
	case VM_INST_JUMP_NZ:
	{
		assert(inst->arguments[0]->type.size == 4);

		unsigned char sourceReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);

		// Check if one side of the jump is fall-through
		if(lowBlock->vmBlock->nextSibling && lowBlock->vmBlock->nextSibling == inst->arguments[1])
		{
			lowBlock->AddInstruction(ctx, inst->source, rviJmpz, 0, 0, sourceReg, getType<VmBlock>(inst->arguments[2]));
		}
		else
		{
			lowBlock->AddInstruction(ctx, inst->source, rviJmpnz, 0, 0, sourceReg, getType<VmBlock>(inst->arguments[1]));

			lowBlock->AddInstruction(ctx, inst->source, rviJmp, 0, 0, 0, getType<VmBlock>(inst->arguments[2]));
		}
	}
		break;
	case VM_INST_CALL:
	{
		assert((unsigned short)inst->type.size == inst->type.size);

		RegVmInstructionCode targetInst = rviNop;

		VmValue *targetContext = NULL;
		VmFunction *targetFunction = NULL;

		unsigned firstArgument = ~0u;

		SmallArray<unsigned char, 32> targetRegs(ctx.allocator);

		if(inst->arguments[0]->type.type == VM_TYPE_FUNCTION_REF)
		{
			GetArgumentRegisters(ctx, lowFunction, lowBlock, targetRegs, inst->arguments[0]);

			targetInst = rviCallPtr;

			firstArgument = 1;
		}
		else
		{
			targetContext = inst->arguments[0];
			targetFunction = getType<VmFunction>(inst->arguments[1]);

			if(VmConstant *context = getType<VmConstant>(targetContext))
			{
				if(context->container)
				{
					LowerConstantIntoBlock(ctx, lowFunction, lowBlock, targetRegs, context);
					targetRegs.push_back(0);
				}
				else
				{
					targetRegs.push_back(0);
					targetRegs.push_back(0);
				}
			}
			else
			{
				GetArgumentRegisters(ctx, lowFunction, lowBlock, targetRegs, targetContext);
				targetRegs.push_back(0);
			}

			targetInst = rviCall;

			firstArgument = 2;
		}

		for(unsigned i = firstArgument; i < inst->arguments.size(); i++)
		{
			VmValue *argument = inst->arguments[i];

			if(argument->type.size == 0)
			{
				lowBlock->AddInstruction(ctx, inst->source, rviPushImm, 0, 0, 0, 0u);
			}
			else
			{
				if(TryLowerConstantPushIntoBlock(ctx, lowBlock, argument))
					continue;

				SmallArray<unsigned char, 32> argumentRegs(ctx.allocator);
				GetArgumentRegisters(ctx, lowFunction, lowBlock, argumentRegs, argument);

				if(argument->type.type == VM_TYPE_INT || (NULLC_PTR_SIZE == 4 && argument->type.type == VM_TYPE_POINTER))
				{
					lowBlock->AddInstruction(ctx, inst->source, rviPush, 0, 0, argumentRegs[0]);
				}
				else if(argument->type.type == VM_TYPE_LONG || (NULLC_PTR_SIZE == 8 && argument->type.type == VM_TYPE_POINTER))
				{
					lowBlock->AddInstruction(ctx, inst->source, rviPushQword, 0, 0, argumentRegs[0]);
				}
				else if(argument->type.type == VM_TYPE_DOUBLE)
				{
					lowBlock->AddInstruction(ctx, inst->source, rviPushQword, 0, 0, argumentRegs[0]);
				}
				else if(argument->type.type == VM_TYPE_FUNCTION_REF || argument->type.type == VM_TYPE_ARRAY_REF)
				{
					if(NULLC_PTR_SIZE == 8)
					{
						lowBlock->AddInstruction(ctx, inst->source, rviPushQword, 0, 0, argumentRegs[0]);
						lowBlock->AddInstruction(ctx, inst->source, rviPush, 0, 0, argumentRegs[1]);
					}
					else
					{
						lowBlock->AddInstruction(ctx, inst->source, rviPush, 0, 0, argumentRegs[0]);
						lowBlock->AddInstruction(ctx, inst->source, rviPush, 0, 0, argumentRegs[1]);
					}
				}
				else if(argument->type.type == VM_TYPE_AUTO_REF)
				{
					if(NULLC_PTR_SIZE == 8)
					{
						lowBlock->AddInstruction(ctx, inst->source, rviPush, 0, 0, argumentRegs[0]);
						lowBlock->AddInstruction(ctx, inst->source, rviPushQword, 0, 0, argumentRegs[1]);
					}
					else
					{
						lowBlock->AddInstruction(ctx, inst->source, rviPush, 0, 0, argumentRegs[0]);
						lowBlock->AddInstruction(ctx, inst->source, rviPush, 0, 0, argumentRegs[1]);
					}
				}
				else if(argument->type.type == VM_TYPE_AUTO_ARRAY)
				{
					if(NULLC_PTR_SIZE == 8)
					{
						lowBlock->AddInstruction(ctx, inst->source, rviPush, 0, 0, argumentRegs[0]);
						lowBlock->AddInstruction(ctx, inst->source, rviPushQword, 0, 0, argumentRegs[1]);
						lowBlock->AddInstruction(ctx, inst->source, rviPush, 0, 0, argumentRegs[2]);
					}
					else
					{
						lowBlock->AddInstruction(ctx, inst->source, rviPush, 0, 0, argumentRegs[0]);
						lowBlock->AddInstruction(ctx, inst->source, rviPush, 0, 0, argumentRegs[1]);
						lowBlock->AddInstruction(ctx, inst->source, rviPush, 0, 0, argumentRegs[2]);
					}
				}
				else if(argument->type.type == VM_TYPE_STRUCT)
				{
					unsigned remainingSize = argument->type.size;

					for(unsigned i = 0; i < argumentRegs.size(); i++)
					{
						if(remainingSize == 4)
						{
							lowBlock->AddInstruction(ctx, inst->source, rviPush, 0, 0, argumentRegs[i]);

							remainingSize -= 4;
						}
						else
						{
							lowBlock->AddInstruction(ctx, inst->source, rviPushQword, 0, 0, argumentRegs[i]);

							remainingSize -= 8;
						}
					}

					assert(remainingSize == 0);
				}
			}
		}

		if(VmConstant *context = getType<VmConstant>(targetContext))
		{
			if(context->container)
			{
				if(NULLC_PTR_SIZE == 8)
					lowBlock->AddInstruction(ctx, inst->source, rviPushQword, 0, 0, targetRegs[0]);
				else
					lowBlock->AddInstruction(ctx, inst->source, rviPush, 0, 0, targetRegs[0]);
			}
			else
			{
				if(NULLC_PTR_SIZE == 8)
					lowBlock->AddInstruction(ctx, inst->source, rviPushImmq, 0, 0, 0, context->iValue);
				else
					lowBlock->AddInstruction(ctx, inst->source, rviPushImm, 0, 0, 0, context->iValue);
			}
		}
		else
		{
			if(NULLC_PTR_SIZE == 8)
				lowBlock->AddInstruction(ctx, inst->source, rviPushQword, 0, 0, targetRegs[0]);
			else
				lowBlock->AddInstruction(ctx, inst->source, rviPush, 0, 0, targetRegs[0]);
		}

		bool fakeUser = false;

		if(inst->type.type != VM_TYPE_VOID && inst->users.empty())
		{
			fakeUser = true;
			inst->users.push_back(NULL);
		}

		if(inst->type.type == VM_TYPE_VOID)
		{
			lowBlock->AddInstruction(ctx, inst->source, targetInst, 0, rvrVoid, targetRegs[1], targetFunction);
		}
		else if(inst->type.type == VM_TYPE_INT || (NULLC_PTR_SIZE == 4 && inst->type.type == VM_TYPE_POINTER))
		{
			unsigned char targetReg = lowFunction->AllocateRegister(inst);

			lowBlock->AddInstruction(ctx, inst->source, targetInst, targetReg, rvrInt, targetRegs[1], targetFunction);
		}
		else if(inst->type.type == VM_TYPE_DOUBLE || inst->type.type == VM_TYPE_LONG || (NULLC_PTR_SIZE == 8 && inst->type.type == VM_TYPE_POINTER))
		{
			unsigned char targetReg = lowFunction->AllocateRegister(inst);

			lowBlock->AddInstruction(ctx, inst->source, targetInst, targetReg, (unsigned char)(inst->type.type == VM_TYPE_DOUBLE ? rvrDouble : rvrLong), targetRegs[1], targetFunction);
		}
		else if(inst->type.type == VM_TYPE_FUNCTION_REF || inst->type.type == VM_TYPE_ARRAY_REF)
		{
			lowBlock->AddInstruction(ctx, inst->source, targetInst, 0, rvrStruct, targetRegs[1], targetFunction);

			unsigned char regA = lowFunction->AllocateRegister(inst, 0, false);
			unsigned char regB = lowFunction->AllocateRegister(inst, 1, true);

			if(NULLC_PTR_SIZE == 8)
			{
				lowBlock->AddInstruction(ctx, inst->source, rviPopq, regA, 0, 0, 0u);
				lowBlock->AddInstruction(ctx, inst->source, rviPop, regB, 0, 0, 2u);
			}
			else
			{
				lowBlock->AddInstruction(ctx, inst->source, rviPop, regA, 0, 0, 0u);
				lowBlock->AddInstruction(ctx, inst->source, rviPop, regB, 0, 0, 1u);
			}
		}
		else if(inst->type.type == VM_TYPE_AUTO_REF)
		{
			lowBlock->AddInstruction(ctx, inst->source, targetInst, 0, rvrStruct, targetRegs[1], targetFunction);

			unsigned char regA = lowFunction->AllocateRegister(inst, 0, false);
			unsigned char regB = lowFunction->AllocateRegister(inst, 1, true);

			if(NULLC_PTR_SIZE == 8)
			{
				lowBlock->AddInstruction(ctx, inst->source, rviPop, regA, 0, 0, 0u);
				lowBlock->AddInstruction(ctx, inst->source, rviPopq, regB, 0, 0, 1u);
			}
			else
			{
				lowBlock->AddInstruction(ctx, inst->source, rviPop, regA, 0, 0, 0u);
				lowBlock->AddInstruction(ctx, inst->source, rviPop, regB, 0, 0, 1u);
			}
		}
		else if(inst->type.type == VM_TYPE_AUTO_ARRAY)
		{
			lowBlock->AddInstruction(ctx, inst->source, targetInst, 0, rvrStruct, targetRegs[1], targetFunction);

			unsigned char regA = lowFunction->AllocateRegister(inst, 0, false);
			unsigned char regB = lowFunction->AllocateRegister(inst, 1, false);
			unsigned char regC = lowFunction->AllocateRegister(inst, 2, true);

			if(NULLC_PTR_SIZE == 8)
			{
				lowBlock->AddInstruction(ctx, inst->source, rviPop, regA, 0, 0, 0u);
				lowBlock->AddInstruction(ctx, inst->source, rviPopq, regB, 0, 0, 1u);
				lowBlock->AddInstruction(ctx, inst->source, rviPop, regC, 0, 0, 3u);
			}
			else
			{
				lowBlock->AddInstruction(ctx, inst->source, rviPop, regA, 0, 0, 0u);
				lowBlock->AddInstruction(ctx, inst->source, rviPop, regB, 0, 0, 1u);
				lowBlock->AddInstruction(ctx, inst->source, rviPop, regC, 0, 0, 2u);
			}
		}
		else if(inst->type.type == VM_TYPE_STRUCT)
		{
			lowBlock->AddInstruction(ctx, inst->source, targetInst, 0, rvrStruct, targetRegs[1], targetFunction);

			unsigned remainingSize = inst->type.size;

			for(unsigned i = 0; i < (remainingSize + 4) / 8; i++)
				lowFunction->AllocateRegister(inst, i, i + 1 == (remainingSize + 4) / 8);

			unsigned index = 0;

			while(remainingSize != 0)
			{
				if(remainingSize == 4)
				{
					lowBlock->AddInstruction(ctx, inst->source, rviPop, inst->regVmRegisters[index], 0, 0, (inst->type.size - remainingSize) / 4);

					remainingSize -= 4;
				}
				else
				{
					lowBlock->AddInstruction(ctx, inst->source, rviPopq, inst->regVmRegisters[index], 0, 0, (inst->type.size - remainingSize) / 4);

					remainingSize -= 8;
				}

				index++;
			}
		}

		if(fakeUser)
		{
			for(unsigned i = 0; i < inst->regVmRegisters.size(); i++)
				lowFunction->FreeRegister(inst->regVmRegisters[i]);

			inst->users.clear();
		}
	}
	break;
	case VM_INST_RETURN:
	case VM_INST_YIELD:
	{
		if(!inst->arguments.empty())
		{
			VmValue *result = inst->arguments[0];

			RegVmReturnType resultType = rvrVoid;

			if(result->type.type == VM_TYPE_INT || (NULLC_PTR_SIZE == 4 && result->type.type == VM_TYPE_POINTER))
				resultType = rvrInt;
			else if(result->type.type == VM_TYPE_LONG || (NULLC_PTR_SIZE == 8 && result->type.type == VM_TYPE_POINTER))
				resultType = rvrLong;
			else if(result->type.type == VM_TYPE_DOUBLE)
				resultType = rvrDouble;
			else if(result->type.type == VM_TYPE_FUNCTION_REF || result->type.type == VM_TYPE_ARRAY_REF)
				resultType = rvrStruct;
			else if(result->type.type == VM_TYPE_AUTO_REF)
				resultType = rvrStruct;
			else if(result->type.type == VM_TYPE_AUTO_ARRAY)
				resultType = rvrStruct;
			else if(result->type.type == VM_TYPE_STRUCT)
				resultType = rvrStruct;

			if(!TryLowerConstantPushIntoBlock(ctx, lowBlock, result))
			{
				SmallArray<unsigned char, 32> resultRegs(ctx.allocator);
				GetArgumentRegisters(ctx, lowFunction, lowBlock, resultRegs, result);

				if(result->type.type == VM_TYPE_INT || (NULLC_PTR_SIZE == 4 && result->type.type == VM_TYPE_POINTER))
				{
					lowBlock->AddInstruction(ctx, inst->source, rviPush, 0, 0, resultRegs[0]);
				}
				else if(result->type.type == VM_TYPE_LONG || (NULLC_PTR_SIZE == 8 && result->type.type == VM_TYPE_POINTER))
				{
					lowBlock->AddInstruction(ctx, inst->source, rviPushQword, 0, 0, resultRegs[0]);
				}
				else if(result->type.type == VM_TYPE_DOUBLE)
				{
					lowBlock->AddInstruction(ctx, inst->source, rviPushQword, 0, 0, resultRegs[0]);
				}
				else if(result->type.type == VM_TYPE_FUNCTION_REF || result->type.type == VM_TYPE_ARRAY_REF)
				{
					if(NULLC_PTR_SIZE == 8)
					{
						lowBlock->AddInstruction(ctx, inst->source, rviPushQword, 0, 0, resultRegs[0]);
						lowBlock->AddInstruction(ctx, inst->source, rviPush, 0, 0, resultRegs[1]);
					}
					else
					{
						lowBlock->AddInstruction(ctx, inst->source, rviPush, 0, 0, resultRegs[0]);
						lowBlock->AddInstruction(ctx, inst->source, rviPush, 0, 0, resultRegs[1]);
					}
				}
				else if(result->type.type == VM_TYPE_AUTO_REF)
				{
					if(NULLC_PTR_SIZE == 8)
					{
						lowBlock->AddInstruction(ctx, inst->source, rviPush, 0, 0, resultRegs[0]);
						lowBlock->AddInstruction(ctx, inst->source, rviPushQword, 0, 0, resultRegs[1]);
					}
					else
					{
						lowBlock->AddInstruction(ctx, inst->source, rviPush, 0, 0, resultRegs[0]);
						lowBlock->AddInstruction(ctx, inst->source, rviPush, 0, 0, resultRegs[1]);
					}
				}
				else if(result->type.type == VM_TYPE_AUTO_ARRAY)
				{
					if(NULLC_PTR_SIZE == 8)
					{
						lowBlock->AddInstruction(ctx, inst->source, rviPush, 0, 0, resultRegs[0]);
						lowBlock->AddInstruction(ctx, inst->source, rviPushQword, 0, 0, resultRegs[1]);
						lowBlock->AddInstruction(ctx, inst->source, rviPush, 0, 0, resultRegs[2]);
					}
					else
					{
						lowBlock->AddInstruction(ctx, inst->source, rviPush, 0, 0, resultRegs[0]);
						lowBlock->AddInstruction(ctx, inst->source, rviPush, 0, 0, resultRegs[1]);
						lowBlock->AddInstruction(ctx, inst->source, rviPush, 0, 0, resultRegs[2]);
					}
				}
				else if(result->type.type == VM_TYPE_STRUCT)
				{
					unsigned remainingSize = result->type.size;

					for(unsigned i = 0; i < resultRegs.size(); i++)
					{
						if(remainingSize == 4)
						{
							lowBlock->AddInstruction(ctx, inst->source, rviPush, 0, 0, resultRegs[i]);

							remainingSize -= 4;
						}
						else
						{
							lowBlock->AddInstruction(ctx, inst->source, rviPushQword, 0, 0, resultRegs[i]);

							remainingSize -= 8;
						}
					}
				}
			}

			if(result->type.structType && (isType<TypeRef>(result->type.structType) || isType<TypeUnsizedArray>(result->type.structType)))
				lowBlock->AddInstruction(ctx, inst->source, rviCheckRet, 0, 0, 0, result->type.structType->typeIndex);

			lowBlock->AddInstruction(ctx, inst->source, rviReturn, 0, (unsigned char)resultType, 0, result->type.size);
		}
		else
		{
			lowBlock->AddInstruction(ctx, inst->source, rviReturn, 0, rvrVoid, 0);
		}
	}
	break;
	case VM_INST_ADD:
	{
		unsigned char lhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);

		if(VmConstant *constant = getType<VmConstant>(inst->arguments[1]))
		{
			if((inst->type == VmType::Int || (inst->type.type == VM_TYPE_POINTER && NULLC_PTR_SIZE == 4)) && constant->type == VmType::Int)
			{
				unsigned char targetReg = lowFunction->AllocateRegister(inst);

				lowBlock->AddInstruction(ctx, inst->source, rviAddImm, targetReg, lhsReg, 0, constant->iValue);
				break;
			}
			else if((inst->type == VmType::Long || (inst->type.type == VM_TYPE_POINTER && NULLC_PTR_SIZE == 8)) && (constant->type == VmType::Int || inst->type == VmType::Long) && int(constant->lValue) == constant->lValue)
			{
				unsigned char targetReg = lowFunction->AllocateRegister(inst);

				if(constant->type == VmType::Int)
					lowBlock->AddInstruction(ctx, inst->source, rviAddImml, targetReg, lhsReg, 0, constant->iValue);
				else if(inst->type == VmType::Long)
					lowBlock->AddInstruction(ctx, inst->source, rviAddImml, targetReg, lhsReg, 0, int(constant->lValue));
				else
					assert(!"unknown type");
				break;
			}
		}

		unsigned char rhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[1]);
		unsigned char targetReg = lowFunction->AllocateRegister(inst);

		if(inst->type == VmType::Int || (inst->type.type == VM_TYPE_POINTER && NULLC_PTR_SIZE == 4))
			lowBlock->AddInstruction(ctx, inst->source, rviAdd, targetReg, lhsReg, rhsReg);
		else if(inst->type == VmType::Double)
			lowBlock->AddInstruction(ctx, inst->source, rviAddd, targetReg, lhsReg, rhsReg);
		else if(inst->type == VmType::Long || (inst->type.type == VM_TYPE_POINTER && NULLC_PTR_SIZE == 8))
			lowBlock->AddInstruction(ctx, inst->source, rviAddl, targetReg, lhsReg, rhsReg);
		else
			assert(!"unknown type");
	}
	break;
	case VM_INST_SUB:
	{
		unsigned char lhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);

		if(VmConstant *constant = getType<VmConstant>(inst->arguments[1]))
		{
			if((inst->type == VmType::Int || (inst->type.type == VM_TYPE_POINTER && NULLC_PTR_SIZE == 4)) && constant->type == VmType::Int)
			{
				unsigned char targetReg = lowFunction->AllocateRegister(inst);

				lowBlock->AddInstruction(ctx, inst->source, rviAddImm, targetReg, lhsReg, 0, -constant->iValue);
				break;
			}
			else if((inst->type == VmType::Long || (inst->type.type == VM_TYPE_POINTER && NULLC_PTR_SIZE == 8)) && (constant->type == VmType::Int || inst->type == VmType::Long) && int(constant->lValue) == constant->lValue)
			{
				unsigned char targetReg = lowFunction->AllocateRegister(inst);

				if(constant->type == VmType::Int)
					lowBlock->AddInstruction(ctx, inst->source, rviAddImml, targetReg, lhsReg, 0, -constant->iValue);
				else if(inst->type == VmType::Long)
					lowBlock->AddInstruction(ctx, inst->source, rviAddImml, targetReg, lhsReg, 0, -int(constant->lValue));
				else
					assert(!"unknown type");
				break;
			}
		}

		unsigned char rhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[1]);
		unsigned char targetReg = lowFunction->AllocateRegister(inst);

		if(inst->type == VmType::Int)
			lowBlock->AddInstruction(ctx, inst->source, rviSub, targetReg, lhsReg, rhsReg);
		else if(inst->type == VmType::Double)
			lowBlock->AddInstruction(ctx, inst->source, rviSubd, targetReg, lhsReg, rhsReg);
		else if(inst->type == VmType::Long)
			lowBlock->AddInstruction(ctx, inst->source, rviSubl, targetReg, lhsReg, rhsReg);
		else
			assert(!"unknown type");
	}
	break;
	case VM_INST_MUL:
	{
		unsigned char lhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
		unsigned char rhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[1]);
		unsigned char targetReg = lowFunction->AllocateRegister(inst);

		if(inst->type == VmType::Int)
			lowBlock->AddInstruction(ctx, inst->source, rviMul, targetReg, lhsReg, rhsReg);
		else if(inst->type == VmType::Double)
			lowBlock->AddInstruction(ctx, inst->source, rviMuld, targetReg, lhsReg, rhsReg);
		else if(inst->type == VmType::Long)
			lowBlock->AddInstruction(ctx, inst->source, rviMull, targetReg, lhsReg, rhsReg);
		else
			assert(!"unknown type");
	}
		break;
	case VM_INST_DIV:
	{
		unsigned char lhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
		unsigned char rhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[1]);
		unsigned char targetReg = lowFunction->AllocateRegister(inst);

		if(inst->type == VmType::Int)
			lowBlock->AddInstruction(ctx, inst->source, rviDiv, targetReg, lhsReg, rhsReg);
		else if(inst->type == VmType::Double)
			lowBlock->AddInstruction(ctx, inst->source, rviDivd, targetReg, lhsReg, rhsReg);
		else if(inst->type == VmType::Long)
			lowBlock->AddInstruction(ctx, inst->source, rviDivl, targetReg, lhsReg, rhsReg);
		else
			assert(!"unknown type");
	}
		break;
	case VM_INST_POW:
	{
		unsigned char lhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
		unsigned char rhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[1]);
		unsigned char targetReg = lowFunction->AllocateRegister(inst);

		if(inst->type == VmType::Int)
			lowBlock->AddInstruction(ctx, inst->source, rviPow, targetReg, lhsReg, rhsReg);
		else if(inst->type == VmType::Double)
			lowBlock->AddInstruction(ctx, inst->source, rviPowd, targetReg, lhsReg, rhsReg);
		else if(inst->type == VmType::Long)
			lowBlock->AddInstruction(ctx, inst->source, rviPowl, targetReg, lhsReg, rhsReg);
		else
			assert(!"unknown type");
	}
		break;
	case VM_INST_MOD:
	{
		unsigned char lhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
		unsigned char rhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[1]);
		unsigned char targetReg = lowFunction->AllocateRegister(inst);

		if(inst->type == VmType::Int)
			lowBlock->AddInstruction(ctx, inst->source, rviMod, targetReg, lhsReg, rhsReg);
		else if(inst->type == VmType::Double)
			lowBlock->AddInstruction(ctx, inst->source, rviModd, targetReg, lhsReg, rhsReg);
		else if(inst->type == VmType::Long)
			lowBlock->AddInstruction(ctx, inst->source, rviModl, targetReg, lhsReg, rhsReg);
		else
			assert(!"unknown type");
	}
		break;
	case VM_INST_LESS:
	{
		unsigned char lhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
		unsigned char rhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[1]);
		unsigned char targetReg = lowFunction->AllocateRegister(inst);

		if(inst->arguments[0]->type == VmType::Int)
			lowBlock->AddInstruction(ctx, inst->source, rviLess, targetReg, lhsReg, rhsReg);
		else if(inst->arguments[0]->type == VmType::Double)
			lowBlock->AddInstruction(ctx, inst->source, rviLessd, targetReg, lhsReg, rhsReg);
		else if(inst->arguments[0]->type == VmType::Long)
			lowBlock->AddInstruction(ctx, inst->source, rviLessl, targetReg, lhsReg, rhsReg);
		else
			assert(!"unknown type");
	}
		break;
	case VM_INST_GREATER:
	{
		unsigned char lhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
		unsigned char rhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[1]);
		unsigned char targetReg = lowFunction->AllocateRegister(inst);

		if(inst->arguments[0]->type == VmType::Int)
			lowBlock->AddInstruction(ctx, inst->source, rviGreater, targetReg, lhsReg, rhsReg);
		else if(inst->arguments[0]->type == VmType::Double)
			lowBlock->AddInstruction(ctx, inst->source, rviGreaterd, targetReg, lhsReg, rhsReg);
		else if(inst->arguments[0]->type == VmType::Long)
			lowBlock->AddInstruction(ctx, inst->source, rviGreaterl, targetReg, lhsReg, rhsReg);
		else
			assert(!"unknown type");
	}
		break;
	case VM_INST_LESS_EQUAL:
	{
		unsigned char lhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
		unsigned char rhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[1]);
		unsigned char targetReg = lowFunction->AllocateRegister(inst);

		if(inst->arguments[0]->type == VmType::Int)
			lowBlock->AddInstruction(ctx, inst->source, rviLequal, targetReg, lhsReg, rhsReg);
		else if(inst->arguments[0]->type == VmType::Double)
			lowBlock->AddInstruction(ctx, inst->source, rviLequald, targetReg, lhsReg, rhsReg);
		else if(inst->arguments[0]->type == VmType::Long)
			lowBlock->AddInstruction(ctx, inst->source, rviLequall, targetReg, lhsReg, rhsReg);
		else
			assert(!"unknown type");
	}
		break;
	case VM_INST_GREATER_EQUAL:
	{
		unsigned char lhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
		unsigned char rhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[1]);
		unsigned char targetReg = lowFunction->AllocateRegister(inst);

		if(inst->arguments[0]->type == VmType::Int)
			lowBlock->AddInstruction(ctx, inst->source, rviGequal, targetReg, lhsReg, rhsReg);
		else if(inst->arguments[0]->type == VmType::Double)
			lowBlock->AddInstruction(ctx, inst->source, rviGequald, targetReg, lhsReg, rhsReg);
		else if(inst->arguments[0]->type == VmType::Long)
			lowBlock->AddInstruction(ctx, inst->source, rviGequall, targetReg, lhsReg, rhsReg);
		else
			assert(!"unknown type");
	}
		break;
	case VM_INST_EQUAL:
	{
		unsigned char lhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
		unsigned char rhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[1]);
		unsigned char targetReg = lowFunction->AllocateRegister(inst);

		if(inst->arguments[0]->type == VmType::Int)
			lowBlock->AddInstruction(ctx, inst->source, rviEqual, targetReg, lhsReg, rhsReg);
		else if(inst->arguments[0]->type == VmType::Double)
			lowBlock->AddInstruction(ctx, inst->source, rviEquald, targetReg, lhsReg, rhsReg);
		else if(inst->arguments[0]->type == VmType::Long)
			lowBlock->AddInstruction(ctx, inst->source, rviEquall, targetReg, lhsReg, rhsReg);
		else if(inst->arguments[0]->type.type == VM_TYPE_POINTER)
			lowBlock->AddInstruction(ctx, inst->source, NULLC_PTR_SIZE == 4 ? rviEqual : rviEquall, targetReg, lhsReg, rhsReg);
		else
			assert(!"unknown type");
	}
		break;
	case VM_INST_NOT_EQUAL:
	{
		unsigned char lhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
		unsigned char rhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[1]);
		unsigned char targetReg = lowFunction->AllocateRegister(inst);

		if(inst->arguments[0]->type == VmType::Int)
			lowBlock->AddInstruction(ctx, inst->source, rviNequal, targetReg, lhsReg, rhsReg);
		else if(inst->arguments[0]->type == VmType::Double)
			lowBlock->AddInstruction(ctx, inst->source, rviNequald, targetReg, lhsReg, rhsReg);
		else if(inst->arguments[0]->type == VmType::Long)
			lowBlock->AddInstruction(ctx, inst->source, rviNequall, targetReg, lhsReg, rhsReg);
		else if(inst->arguments[0]->type.type == VM_TYPE_POINTER)
			lowBlock->AddInstruction(ctx, inst->source, NULLC_PTR_SIZE == 4 ? rviNequal : rviNequall, targetReg, lhsReg, rhsReg);
		else
			assert(!"unknown type");
	}
		break;
	case VM_INST_SHL:
	{
		unsigned char lhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
		unsigned char rhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[1]);
		unsigned char targetReg = lowFunction->AllocateRegister(inst);

		if(inst->type == VmType::Int)
			lowBlock->AddInstruction(ctx, inst->source, rviShl, targetReg, lhsReg, rhsReg);
		else if(inst->type == VmType::Long)
			lowBlock->AddInstruction(ctx, inst->source, rviShll, targetReg, lhsReg, rhsReg);
		else
			assert(!"unknown type");
	}
		break;
	case VM_INST_SHR:
	{
		unsigned char lhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
		unsigned char rhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[1]);
		unsigned char targetReg = lowFunction->AllocateRegister(inst);

		if(inst->type == VmType::Int)
			lowBlock->AddInstruction(ctx, inst->source, rviShr, targetReg, lhsReg, rhsReg);
		else if(inst->type == VmType::Long)
			lowBlock->AddInstruction(ctx, inst->source, rviShrl, targetReg, lhsReg, rhsReg);
		else
			assert(!"unknown type");
	}
		break;
	case VM_INST_BIT_AND:
	{
		unsigned char lhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
		unsigned char rhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[1]);
		unsigned char targetReg = lowFunction->AllocateRegister(inst);

		if(inst->type == VmType::Int)
			lowBlock->AddInstruction(ctx, inst->source, rviBitAnd, targetReg, lhsReg, rhsReg);
		else if(inst->type == VmType::Long)
			lowBlock->AddInstruction(ctx, inst->source, rviBitAndl, targetReg, lhsReg, rhsReg);
		else
			assert(!"unknown type");
	}
		break;
	case VM_INST_BIT_OR:
	{
		unsigned char lhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
		unsigned char rhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[1]);
		unsigned char targetReg = lowFunction->AllocateRegister(inst);

		if(inst->type == VmType::Int)
			lowBlock->AddInstruction(ctx, inst->source, rviBitOr, targetReg, lhsReg, rhsReg);
		else if(inst->type == VmType::Long)
			lowBlock->AddInstruction(ctx, inst->source, rviBitOrl, targetReg, lhsReg, rhsReg);
		else
			assert(!"unknown type");
	}
		break;
	case VM_INST_BIT_XOR:
	{
		unsigned char lhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
		unsigned char rhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[1]);
		unsigned char targetReg = lowFunction->AllocateRegister(inst);

		if(inst->type == VmType::Int)
			lowBlock->AddInstruction(ctx, inst->source, rviBitXor, targetReg, lhsReg, rhsReg);
		else if(inst->type == VmType::Long)
			lowBlock->AddInstruction(ctx, inst->source, rviBitXorl, targetReg, lhsReg, rhsReg);
		else
			assert(!"unknown type");
	}
		break;
	case VM_INST_LOG_XOR:
	{
		unsigned char lhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
		unsigned char rhsReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[1]);
		unsigned char targetReg = lowFunction->AllocateRegister(inst);

		if(inst->arguments[0]->type == VmType::Int)
			lowBlock->AddInstruction(ctx, inst->source, rviLogXor, targetReg, lhsReg, rhsReg);
		else if(inst->arguments[0]->type == VmType::Long)
			lowBlock->AddInstruction(ctx, inst->source, rviLogXorl);
		else
			assert(!"unknown type");
	}
		break;
	case VM_INST_NEG:
	{
		unsigned char sourceReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
		unsigned char targetReg = lowFunction->AllocateRegister(inst);

		if(inst->type == VmType::Int)
			lowBlock->AddInstruction(ctx, inst->source, rviNeg, targetReg, 0, sourceReg);
		else if(inst->type == VmType::Double)
			lowBlock->AddInstruction(ctx, inst->source, rviNegd, targetReg, 0, sourceReg);
		else if(inst->type == VmType::Long)
			lowBlock->AddInstruction(ctx, inst->source, rviNegl, targetReg, 0, sourceReg);
		else
			assert(!"unknown type");
	}
		break;
	case VM_INST_BIT_NOT:
	{
		unsigned char sourceReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
		unsigned char targetReg = lowFunction->AllocateRegister(inst);

		if(inst->type == VmType::Int)
			lowBlock->AddInstruction(ctx, inst->source, rviBitNot, targetReg, 0, sourceReg);
		else if(inst->type == VmType::Long)
			lowBlock->AddInstruction(ctx, inst->source, rviBitNotl, targetReg, 0, sourceReg);
		else
			assert(!"unknown type");
	}
		break;
	case VM_INST_LOG_NOT:
	{
		unsigned char sourceReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
		unsigned char targetReg = lowFunction->AllocateRegister(inst);

		if(inst->arguments[0]->type == VmType::Int)
			lowBlock->AddInstruction(ctx, inst->source, rviLogNot, targetReg, 0, sourceReg);
		else if(inst->arguments[0]->type == VmType::Long)
			lowBlock->AddInstruction(ctx, inst->source, rviLogNotl, targetReg, 0, sourceReg);
		else if(inst->arguments[0]->type.type == VM_TYPE_POINTER)
			lowBlock->AddInstruction(ctx, inst->source, NULLC_PTR_SIZE == 4 ? rviLogNot : rviLogNotl, targetReg, 0, sourceReg);
		else
			assert(!"unknown type");
	}
		break;
	case VM_INST_CONVERT_POINTER:
	{
		VmValue *pointer = inst->arguments[0];
		VmConstant *typeIndex = getType<VmConstant>(inst->arguments[1]);

		assert(pointer->type.type == VM_TYPE_AUTO_REF);
		assert(typeIndex);

		SmallArray<unsigned char, 32> sourceRegs(ctx.allocator);
		GetArgumentRegisters(ctx, lowFunction, lowBlock, sourceRegs, pointer);

		bool fakeUser = false;

		if(inst->users.empty())
		{
			fakeUser = true;
			inst->users.push_back(NULL);
		}

		unsigned char targetReg = lowFunction->AllocateRegister(inst);

		lowBlock->AddInstruction(ctx, inst->source, rviConvertPtr, targetReg, sourceRegs[0], sourceRegs[1], typeIndex->iValue);

		if(fakeUser)
		{
			for(unsigned i = 0; i < inst->regVmRegisters.size(); i++)
				lowFunction->FreeRegister(inst->regVmRegisters[i]);

			inst->users.clear();
		}
	}
	break;
	case VM_INST_ABORT_NO_RETURN:
		lowBlock->AddInstruction(ctx, inst->source, rviReturn, 0, rvrError, 0);
		break;
	case VM_INST_CONSTRUCT:
		if(inst->type.type == VM_TYPE_FUNCTION_REF)
		{
			unsigned char ptrReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
			unsigned char idReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[1]);

			if(!lowFunction->TransferRegisterTo(inst, ptrReg))
			{
				unsigned char copyReg = lowFunction->AllocateRegister(inst, 0, false);

				lowBlock->AddInstruction(ctx, inst->source, rviMov, copyReg, 0, ptrReg);
			}

			if(!lowFunction->TransferRegisterTo(inst, idReg))
			{
				unsigned char copyReg = lowFunction->AllocateRegister(inst, 1, true);

				lowBlock->AddInstruction(ctx, inst->source, rviMov, copyReg, 0, idReg);
			}
		}
		else if(inst->type.type == VM_TYPE_ARRAY_REF)
		{
			unsigned char ptrReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
			unsigned char lenReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[1]);

			if(!lowFunction->TransferRegisterTo(inst, ptrReg))
			{
				unsigned char copyReg = lowFunction->AllocateRegister(inst, 0, false);

				lowBlock->AddInstruction(ctx, inst->source, rviMov, copyReg, 0, ptrReg);
			}

			if(!lowFunction->TransferRegisterTo(inst, lenReg))
			{
				unsigned char copyReg = lowFunction->AllocateRegister(inst, 1, true);

				lowBlock->AddInstruction(ctx, inst->source, rviMov, copyReg, 0, lenReg);
			}
		}
		else if(inst->type.type == VM_TYPE_AUTO_REF)
		{
			unsigned char typeReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
			unsigned char ptrReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[1]);

			if(!lowFunction->TransferRegisterTo(inst, typeReg))
			{
				unsigned char copyReg = lowFunction->AllocateRegister(inst, 0, false);

				lowBlock->AddInstruction(ctx, inst->source, rviMov, copyReg, 0, typeReg);
			}

			if(!lowFunction->TransferRegisterTo(inst, ptrReg))
			{
				unsigned char copyReg = lowFunction->AllocateRegister(inst, 1, true);

				lowBlock->AddInstruction(ctx, inst->source, rviMov, copyReg, 0, ptrReg);
			}
		}
		else if(inst->type.type == VM_TYPE_AUTO_ARRAY)
		{
			if(inst->arguments.size() == 2)
			{
				unsigned char typeReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);

				SmallArray<unsigned char, 32> arrayRegs(ctx.allocator);
				GetArgumentRegisters(ctx, lowFunction, lowBlock, arrayRegs, inst->arguments[1]);

				if(!lowFunction->TransferRegisterTo(inst, typeReg))
				{
					unsigned char copyReg = lowFunction->AllocateRegister(inst, 0, false);

					lowBlock->AddInstruction(ctx, inst->source, rviMov, copyReg, 0, typeReg);
				}

				if(!lowFunction->TransferRegisterTo(inst, arrayRegs[0]))
				{
					unsigned char copyReg = lowFunction->AllocateRegister(inst, 1, false);

					lowBlock->AddInstruction(ctx, inst->source, rviMov, copyReg, 0, arrayRegs[0]);
				}

				if(!lowFunction->TransferRegisterTo(inst, arrayRegs[1]))
				{
					unsigned char copyReg = lowFunction->AllocateRegister(inst, 2, true);

					lowBlock->AddInstruction(ctx, inst->source, rviMov, copyReg, 0, arrayRegs[1]);
				}
			}
			else
			{
				unsigned char typeReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
				unsigned char ptrReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[1]);
				unsigned char sizeReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[2]);

				if(!lowFunction->TransferRegisterTo(inst, typeReg))
				{
					unsigned char copyReg = lowFunction->AllocateRegister(inst, 0, false);

					lowBlock->AddInstruction(ctx, inst->source, rviMov, copyReg, 0, typeReg);
				}

				if(!lowFunction->TransferRegisterTo(inst, ptrReg))
				{
					unsigned char copyReg = lowFunction->AllocateRegister(inst, 1, false);

					lowBlock->AddInstruction(ctx, inst->source, rviMov, copyReg, 0, ptrReg);
				}

				if(!lowFunction->TransferRegisterTo(inst, sizeReg))
				{
					unsigned char copyReg = lowFunction->AllocateRegister(inst, 2, true);

					lowBlock->AddInstruction(ctx, inst->source, rviMov, copyReg, 0, sizeReg);
				}
			}
		}
		else
		{
			assert(!"unknown type");
		}
		break;
	case VM_INST_ARRAY:
	{
		assert(!"must be lagalized");

		assert(inst->arguments.size() <= 8);

		unsigned index = 0;

		for(unsigned i = 0; i < inst->arguments.size(); i++)
		{
			VmValue *argument = inst->arguments[i];

			assert(argument->type.size % 4 == 0);

			if(argument->type.size == 4)
			{
				if(i + 1 < inst->arguments.size())
				{
					unsigned char argReg1 = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[i]);
					unsigned char argReg2 = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[i + 1]);

					unsigned char copyReg = lowFunction->AllocateRegister(inst, index++, true);

					lowBlock->AddInstruction(ctx, inst->source, rviCombinedd, copyReg, argReg1, argReg2);

					i++;
				}
				else
				{
					unsigned char argReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[i]);

					if(!lowFunction->TransferRegisterTo(inst, argReg))
					{
						unsigned char copyReg = lowFunction->AllocateRegister(inst, index++, true);

						lowBlock->AddInstruction(ctx, inst->source, rviMov, copyReg, 0, argReg);
					}
					else
					{
						index++;
					}
				}
			}
			else if(argument->type.size == 8)
			{
				unsigned char argReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[i]);

				if(!lowFunction->TransferRegisterTo(inst, argReg))
				{
					unsigned char copyReg = lowFunction->AllocateRegister(inst, index++, true);

					lowBlock->AddInstruction(ctx, inst->source, rviMov, copyReg, 0, argReg);
				}
				else
				{
					index++;
				}
			}
			else
			{
				SmallArray<unsigned char, 32> sourceRegs(ctx.allocator);
				GetArgumentRegisters(ctx, lowFunction, lowBlock, sourceRegs, inst->arguments[i]);

				for(unsigned k = 0; k < sourceRegs.size(); k++)
				{
					if(!lowFunction->TransferRegisterTo(inst, sourceRegs[k]))
					{
						unsigned char copyReg = lowFunction->AllocateRegister(inst, index++, k + 1 == sourceRegs.size());

						lowBlock->AddInstruction(ctx, inst->source, rviMov, copyReg, 0, sourceRegs[k]);
					}
					else
					{
						index++;
					}
				}
			}
		}
		break;
	}
	case VM_INST_EXTRACT:
		assert(!"invalid instruction");
		break;
	case VM_INST_UNYIELD:
	{
		unsigned char jumpPointReg = GetArgumentRegister(ctx, lowFunction, lowBlock, inst->arguments[0]);
		unsigned char tempReg = lowFunction->GetRegisterForConstant();

		// Check secondary blocks first
		for(unsigned i = 2; i < inst->arguments.size(); i++)
		{
			lowBlock->AddInstruction(ctx, inst->source, rviLoadImm, tempReg, 0, 0, i - 1);
			lowBlock->AddInstruction(ctx, inst->source, rviEqual, tempReg, jumpPointReg, tempReg);
			lowBlock->AddInstruction(ctx, inst->source, rviJmpnz, 0, 0, tempReg, getType<VmBlock>(inst->arguments[i]));
		}

		// jump to entry block by default
		if(!(lowBlock->vmBlock->nextSibling && lowBlock->vmBlock->nextSibling == inst->arguments[1]))
		{
			lowBlock->AddInstruction(ctx, inst->source, rviJmp, 0, 0, 0, getType<VmBlock>(inst->arguments[1]));
		}
	}
		break;
	case VM_INST_BITCAST:
	{
		if((inst->arguments[0]->type.type == VM_TYPE_FUNCTION_REF || inst->arguments[0]->type.type == VM_TYPE_ARRAY_REF || inst->arguments[0]->type.type == VM_TYPE_AUTO_REF || inst->arguments[0]->type.type == VM_TYPE_AUTO_ARRAY) && inst->type.type == VM_TYPE_STRUCT)
		{
			SmallArray<unsigned char, 32> sourceRegs(ctx.allocator);
			GetArgumentRegisters(ctx, lowFunction, lowBlock, sourceRegs, inst->arguments[0]);

			if(NULLC_PTR_SIZE == 8)
			{
				if(inst->arguments[0]->type.type == VM_TYPE_FUNCTION_REF || inst->arguments[0]->type.type == VM_TYPE_ARRAY_REF)
				{
					unsigned index = 0;

					if(!lowFunction->TransferRegisterTo(inst, sourceRegs[0]))
					{
						unsigned char copyReg = lowFunction->AllocateRegister(inst, index++, false);

						lowBlock->AddInstruction(ctx, inst->source, rviMov, copyReg, 0, sourceRegs[0]);
					}
					else
					{
						index++;
					}

					if(!lowFunction->TransferRegisterTo(inst, sourceRegs[1]))
					{
						unsigned char copyReg = lowFunction->AllocateRegister(inst, index, true);

						lowBlock->AddInstruction(ctx, inst->source, rviMov, copyReg, 0, sourceRegs[1]);
					}
				}
				else if(inst->arguments[0]->type.type == VM_TYPE_AUTO_REF)
				{
					unsigned char resRegA = lowFunction->AllocateRegister(inst, 0, false);
					unsigned char resRegB = lowFunction->AllocateRegister(inst, 1, true);

					unsigned char tempRegA = lowFunction->GetRegisterForConstant();

					lowBlock->AddInstruction(ctx, inst->source, rviBreakupdd, tempRegA, resRegB, sourceRegs[1], 32u);

					lowBlock->AddInstruction(ctx, inst->source, rviCombinedd, resRegA, sourceRegs[0], tempRegA);
				}
				else if(inst->arguments[0]->type.type == VM_TYPE_AUTO_ARRAY)
				{
					unsigned char resRegA = lowFunction->AllocateRegister(inst, 0, false);
					unsigned char resRegB = lowFunction->AllocateRegister(inst, 1, true);

					unsigned char tempRegA = lowFunction->GetRegisterForConstant();
					unsigned char tempRegB = lowFunction->GetRegisterForConstant();

					lowBlock->AddInstruction(ctx, inst->source, rviBreakupdd, tempRegA, tempRegB, sourceRegs[1], 32u);

					lowBlock->AddInstruction(ctx, inst->source, rviCombinedd, resRegA, sourceRegs[0], tempRegA);
					lowBlock->AddInstruction(ctx, inst->source, rviCombinedd, resRegB, tempRegB, sourceRegs[2]);
				}
			}
			else
			{
				if(inst->arguments[0]->type.type == VM_TYPE_FUNCTION_REF || inst->arguments[0]->type.type == VM_TYPE_ARRAY_REF || inst->arguments[0]->type.type == VM_TYPE_AUTO_REF)
				{
					unsigned char combineReg = lowFunction->AllocateRegister(inst, 0, true);

					lowBlock->AddInstruction(ctx, inst->source, rviCombinedd, combineReg, sourceRegs[0], sourceRegs[1]);
				}
				else if(inst->arguments[0]->type.type == VM_TYPE_AUTO_ARRAY)
				{
					unsigned char combineReg = lowFunction->AllocateRegister(inst, 0, false);

					lowBlock->AddInstruction(ctx, inst->source, rviCombinedd, combineReg, sourceRegs[0], sourceRegs[1]);

					if(!lowFunction->TransferRegisterTo(inst, sourceRegs[2]))
					{
						unsigned char copyReg = lowFunction->AllocateRegister(inst, 1, true);

						lowBlock->AddInstruction(ctx, inst->source, rviMov, copyReg, 0, sourceRegs[2]);
					}
				}
			}

			break;
		}

		if((inst->type.type == VM_TYPE_FUNCTION_REF || inst->type.type == VM_TYPE_ARRAY_REF || inst->type.type == VM_TYPE_AUTO_REF || inst->type.type == VM_TYPE_AUTO_ARRAY) && inst->arguments[0]->type.type == VM_TYPE_STRUCT)
		{
			SmallArray<unsigned char, 32> sourceRegs(ctx.allocator);
			GetArgumentRegisters(ctx, lowFunction, lowBlock, sourceRegs, inst->arguments[0]);

			if(NULLC_PTR_SIZE == 8)
			{
				if(inst->type.type == VM_TYPE_FUNCTION_REF || inst->type.type == VM_TYPE_ARRAY_REF)
				{
					unsigned index = 0;

					if(!lowFunction->TransferRegisterTo(inst, sourceRegs[0]))
					{
						unsigned char copyReg = lowFunction->AllocateRegister(inst, index++, false);

						lowBlock->AddInstruction(ctx, inst->source, rviMov, copyReg, 0, sourceRegs[0]);
					}
					else
					{
						index++;
					}

					if(!lowFunction->TransferRegisterTo(inst, sourceRegs[1]))
					{
						unsigned char copyReg = lowFunction->AllocateRegister(inst, index, true);

						lowBlock->AddInstruction(ctx, inst->source, rviMov, copyReg, 0, sourceRegs[1]);
					}
				}
				else if(inst->type.type == VM_TYPE_AUTO_REF)
				{
					unsigned char resRegA = lowFunction->AllocateRegister(inst, 0, false);
					unsigned char resRegB = lowFunction->AllocateRegister(inst, 1, true);

					lowBlock->AddInstruction(ctx, inst->source, rviBreakupdd, resRegA, resRegB, sourceRegs[0]);
					lowBlock->AddInstruction(ctx, inst->source, rviCombinedd, resRegB, resRegB, sourceRegs[1]);
				}
				else if(inst->type.type == VM_TYPE_AUTO_ARRAY)
				{
					unsigned char resRegA = lowFunction->AllocateRegister(inst, 0, false);
					unsigned char resRegB = lowFunction->AllocateRegister(inst, 1, false);
					unsigned char resRegC = lowFunction->AllocateRegister(inst, 2, true);

					unsigned char tempReg = lowFunction->GetRegisterForConstant();

					lowBlock->AddInstruction(ctx, inst->source, rviBreakupdd, resRegA, resRegB, sourceRegs[0]);
					lowBlock->AddInstruction(ctx, inst->source, rviBreakupdd, tempReg, resRegC, sourceRegs[1]);
					lowBlock->AddInstruction(ctx, inst->source, rviCombinedd, resRegB, resRegB, tempReg);
				}
			}
			else
			{
				if(inst->type.type == VM_TYPE_FUNCTION_REF || inst->type.type == VM_TYPE_ARRAY_REF || inst->type.type == VM_TYPE_AUTO_REF)
				{
					unsigned char resRegA = lowFunction->AllocateRegister(inst, 0, false);
					unsigned char resRegB = lowFunction->AllocateRegister(inst, 1, true);

					lowBlock->AddInstruction(ctx, inst->source, rviBreakupdd, resRegA, resRegB, sourceRegs[0]);
				}
				else if(inst->type.type == VM_TYPE_AUTO_ARRAY)
				{
					unsigned char resRegA = lowFunction->AllocateRegister(inst, 0, false);
					unsigned char resRegB = lowFunction->AllocateRegister(inst, 1, true);

					lowBlock->AddInstruction(ctx, inst->source, rviBreakupdd, resRegA, resRegB, sourceRegs[0]);

					if(!lowFunction->TransferRegisterTo(inst, sourceRegs[1]))
					{
						unsigned char copyReg = lowFunction->AllocateRegister(inst, 1, true);

						lowBlock->AddInstruction(ctx, inst->source, rviMov, copyReg, 0, sourceRegs[1]);
					}
				}
			}

			break;
		}

		if(inst->arguments[0]->type.type == VM_TYPE_STRUCT && !(inst->type.type == VM_TYPE_INT || inst->type.type == VM_TYPE_LONG || inst->type.type == VM_TYPE_POINTER))
			assert(!"can't bitcast structure to registers");

		SmallArray<unsigned char, 32> sourceRegs(ctx.allocator);
		GetArgumentRegisters(ctx, lowFunction, lowBlock, sourceRegs, inst->arguments[0]);

		unsigned index = 0;

		for(unsigned k = 0; k < sourceRegs.size(); k++)
		{
			if(!lowFunction->TransferRegisterTo(inst, sourceRegs[k]))
			{
				unsigned char copyReg = lowFunction->AllocateRegister(inst, index++, k + 1 == sourceRegs.size());

				lowBlock->AddInstruction(ctx, inst->source, rviMov, copyReg, 0, sourceRegs[k]);
			}
			else
			{
				index++;
			}
		}
	}
		break;
	case VM_INST_MOV:
	{
		SmallArray<unsigned char, 32> sourceRegs(ctx.allocator);
		GetArgumentRegisters(ctx, lowFunction, lowBlock, sourceRegs, inst->arguments[0]);

		for(unsigned k = 0; k < sourceRegs.size(); k++)
		{
			// We can never transfer source register to us, but since we should already have our target register allocated, it may match the source
			unsigned char copyReg = lowFunction->AllocateRegister(inst, k, k + 1 == sourceRegs.size());

			if(copyReg != sourceRegs[k])
				lowBlock->AddInstruction(ctx, inst->source, rviMov, copyReg, 0, sourceRegs[k]);
		}
	}
		break;
	case VM_INST_PHI:
		assert(!inst->regVmRegisters.empty());
		break;
	default:
		assert(!"unknown instruction");
	}

	if(RegVmLoweredInstruction *firstNewInstruction = lastLowered ? lastLowered->nextSibling : lowBlock->firstInstruction)
	{
		for(unsigned i = 0; i < lowFunction->killedRegisters.size(); i++)
			firstNewInstruction->preKillRegisters.push_back(lowFunction->killedRegisters[i]);
	}

	lowFunction->killedRegisters.clear();

	lowFunction->FreeConstantRegisters();
	lowFunction->FreeDelayedRegisters(lowBlock);
}

void CopyRegisters(VmInstruction *target, VmInstruction *source)
{
	assert(target->regVmRegisters.empty());

	for(unsigned i = 0; i < source->regVmRegisters.size(); i++)
		target->regVmRegisters.push_back(source->regVmRegisters[i]);
}

void CompareRegisters(VmInstruction *a, VmInstruction *b)
{
	(void)b;

	assert(a->regVmRegisters.size() == b->regVmRegisters.size());

	for(unsigned i = 0; i < a->regVmRegisters.size(); i++)
		assert(a->regVmRegisters[i] == b->regVmRegisters[i]);
}

void MarkInstructionTree(VmInstruction *inst, unsigned marker)
{
	if(inst->regVmSearchMarker == marker)
		return;

	inst->regVmSearchMarker = marker;

	if(inst->cmd == VM_INST_PHI)
	{
		for(unsigned argumentPos = 0; argumentPos < inst->arguments.size(); argumentPos += 2)
		{
			VmInstruction *argument = getType<VmInstruction>(inst->arguments[argumentPos]);

			MarkInstructionTree(argument, marker);
		}
	}

	for(unsigned userPos = 0; userPos < inst->users.size(); userPos++)
	{
		VmInstruction *instruction = getType<VmInstruction>(inst->users[userPos]);

		if(instruction->cmd == VM_INST_PHI)
			MarkInstructionTree(instruction, marker);
	}
}

bool HasSharedStorageWith(VmInstruction *inst, VmInstruction *other, unsigned marker)
{
	MarkInstructionTree(inst, marker);

	return other->regVmSearchMarker == marker;
}

void UpdateSharedStorage(VmInstruction *inst, unsigned marker)
{
	if(inst->regVmSearchMarker == marker)
		return;

	assert(!inst->regVmRegisters.empty());

	inst->regVmSearchMarker = marker;

	if(inst->cmd == VM_INST_PHI)
	{
		for(unsigned argumentPos = 0; argumentPos < inst->arguments.size(); argumentPos += 2)
		{
			VmInstruction *instruction = getType<VmInstruction>(inst->arguments[argumentPos]);

			if(!instruction->regVmRegisters.empty())
				CompareRegisters(instruction, inst);
			else
				CopyRegisters(instruction, inst);

			UpdateSharedStorage(instruction, marker);
		}
	}

	for(unsigned userPos = 0; userPos < inst->users.size(); userPos++)
	{
		VmInstruction *instruction = getType<VmInstruction>(inst->users[userPos]);

		if(instruction->cmd == VM_INST_PHI)
		{
			if(!instruction->regVmRegisters.empty())
				CompareRegisters(instruction, inst);
			else
				CopyRegisters(instruction, inst);

			UpdateSharedStorage(instruction, marker);
		}
	}
}

RegVmLoweredBlock* RegVmLowerBlock(ExpressionContext &ctx, RegVmLoweredFunction *lowFunction, VmBlock *vmBlock)
{
	RegVmLoweredBlock *lowBlock = new (ctx.get<RegVmLoweredBlock>()) RegVmLoweredBlock(ctx.allocator, vmBlock);

	// TODO: create some kind of wrapper that will handle instruction with storage shared through phi instructions

	// Get entry registers from instructions that already allocated them
	for(unsigned i = 0; i < vmBlock->liveIn.size(); i++)
	{
		VmInstruction *liveIn = vmBlock->liveIn[i];

		bool found = false;

		for(unsigned k = 0; k < i && !found; k++)
		{
			VmInstruction *other = vmBlock->liveIn[k];

			if(HasSharedStorageWith(liveIn, other, lowFunction->nextSearchMarker++))
				found = true;
		}

		if(found)
			continue;

		if(!liveIn->regVmRegisters.empty())
		{
			for(unsigned k = 0; k < liveIn->regVmRegisters.size(); k++)
			{
				unsigned char reg = liveIn->regVmRegisters[k];

				assert(lowFunction->registerUsers[reg] == 0);
				lowFunction->registerUsers[reg]++;

				assert(!lowBlock->entryRegisters.contains(reg));
				lowBlock->entryRegisters.push_back(reg);
			}
		}
	}

	// Check if exit variable registers are already allocated
	for(unsigned i = 0; i < vmBlock->liveOut.size(); i++)
	{
		VmInstruction *liveOut = vmBlock->liveOut[i];

		bool found = false;

		for(unsigned k = 0; k < vmBlock->liveIn.size() && !found; k++)
		{
			VmInstruction *liveIn = vmBlock->liveIn[k];

			if(liveOut == liveIn)
				found = true;
			else if(HasSharedStorageWith(liveOut, liveIn, lowFunction->nextSearchMarker++))
				found = true;
		}

		for(unsigned k = 0; k < vmBlock->liveOut.size() && !found; k++)
		{
			VmInstruction *other = vmBlock->liveOut[k];

			if(liveOut != other && HasSharedStorageWith(liveOut, other, lowFunction->nextSearchMarker++))
				found = true;
		}

		if(found)
			continue;

		if(!liveOut->regVmRegisters.empty())
		{
			for(unsigned k = 0; k < liveOut->regVmRegisters.size(); k++)
			{
				unsigned char reg = liveOut->regVmRegisters[k];

				// Some pre-allocated exit registers might be the same as pre-allocated entry registers, it can happen when entry register has the last use in current block before being used by exit register
				lowFunction->registerUsers[reg]++;
			}
		}
	}

	// Reset free registers state
	lowFunction->freedRegisters.clear();

	for(unsigned i = unsigned(lowFunction->nextRegister == 0 ? 255 : lowFunction->nextRegister) - 1; i >= 2; i--)
	{
		if(lowFunction->registerUsers[i] == 0)
			lowFunction->freedRegisters.push_back((unsigned char)i);
	}

	// Allocate entry registers for instructions that haven't been lowered yet
	for(unsigned i = 0; i < vmBlock->liveIn.size(); i++)
	{
		VmInstruction *liveIn = vmBlock->liveIn[i];

		if(liveIn->regVmRegisters.empty())
		{
			if(liveIn->type.type == VM_TYPE_INT || liveIn->type.type == VM_TYPE_LONG || liveIn->type.type == VM_TYPE_DOUBLE || liveIn->type.type == VM_TYPE_POINTER)
			{
				lowFunction->AllocateRegister(liveIn, 0);
			}
			else if(liveIn->type.type == VM_TYPE_FUNCTION_REF || liveIn->type.type == VM_TYPE_ARRAY_REF || liveIn->type.type == VM_TYPE_AUTO_REF)
			{
				lowFunction->AllocateRegister(liveIn, 1);
				lowFunction->AllocateRegister(liveIn, 1);
			}
			else if(liveIn->type.type == VM_TYPE_AUTO_ARRAY)
			{
				lowFunction->AllocateRegister(liveIn, 0);
				lowFunction->AllocateRegister(liveIn, 1);
				lowFunction->AllocateRegister(liveIn, 2);
			}
			else if(liveIn->type.type == VM_TYPE_STRUCT)
			{
				for(unsigned k = 0; k < (liveIn->type.size + 4) / 8; k++)
					lowFunction->AllocateRegister(liveIn, k);
			}

			for(unsigned k = 0; k < liveIn->regVmRegisters.size(); k++)
			{
				unsigned char reg = liveIn->regVmRegisters[k];

				assert(!lowBlock->entryRegisters.contains(reg));
				lowBlock->entryRegisters.push_back(reg);
			}

			UpdateSharedStorage(liveIn, lowFunction->nextSearchMarker++);
		}
	}

	for(VmInstruction *vmInstruction = vmBlock->firstInstruction; vmInstruction; vmInstruction = vmInstruction->nextSibling)
	{
		LowerInstructionIntoBlock(ctx, lowFunction, lowBlock, vmInstruction);

		if(lowFunction->hasRegisterOverflow)
		{
			lowFunction->registerOverflowLocation = vmInstruction;

			return lowBlock;
		}
	}

	// Collect live out instructions with unique storage
	SmallArray<VmInstruction*, 32> liveOutUniqueStorage(ctx.allocator);

	for(unsigned i = 0; i < vmBlock->liveOut.size(); i++)
	{
		VmInstruction *liveOut = vmBlock->liveOut[i];

		bool found = false;

		for(unsigned k = 0; k < liveOutUniqueStorage.size() && !found; k++)
		{
			VmInstruction *previous = liveOutUniqueStorage[k];

			if(HasSharedStorageWith(liveOut, previous, lowFunction->nextSearchMarker++))
				found = true;
		}

		if(found)
			continue;

		liveOutUniqueStorage.push_back(liveOut);
	}

	// Collect exit registers
	for(unsigned i = 0; i < liveOutUniqueStorage.size(); i++)
	{
		VmInstruction *liveOut = liveOutUniqueStorage[i];

		assert(!liveOut->regVmRegisters.empty());

		UpdateSharedStorage(liveOut, lowFunction->nextSearchMarker++);

		for(unsigned k = 0; k < liveOut->regVmRegisters.size(); k++)
		{
			unsigned char reg = liveOut->regVmRegisters[k];

			lowBlock->exitRegisters.push_back(reg);

			// Might not be the only use
			lowFunction->registerUsers[reg]--;

			if(lowFunction->registerUsers[reg] == 0)
			{
				assert(!lowFunction->freedRegisters.contains(reg));
				lowFunction->freedRegisters.push_back(reg);
			}
		}
	}

	// Free entry registers
	for(unsigned i = 0; i < vmBlock->liveIn.size(); i++)
	{
		VmInstruction *liveIn = vmBlock->liveIn[i];

		bool found = false;

		for(unsigned k = 0; k < vmBlock->liveOut.size() && !found; k++)
		{
			VmInstruction *liveOut = vmBlock->liveOut[k];

			if(liveOut == liveIn)
				found = true;
			else if(HasSharedStorageWith(liveIn, liveOut, lowFunction->nextSearchMarker++))
				found = true;
		}

		if(found)
			continue;

		for(unsigned k = 0; k < liveIn->regVmRegisters.size(); k++)
		{
			unsigned char reg = liveIn->regVmRegisters[k];

			assert(lowFunction->registerUsers[reg] == 1);
			lowFunction->registerUsers[reg]--;

			assert(!lowFunction->freedRegisters.contains(reg));
			lowFunction->freedRegisters.push_back(reg);
		}
	}

	for(unsigned i = 0; i < lowFunction->registerUsers.size(); i++)
	{
		if(lowFunction->registerUsers[i])
			lowBlock->leakedRegisters.push_back((unsigned char)i);
	}

	return lowBlock;
}

RegVmLoweredFunction* RegVmLowerFunction(ExpressionContext &ctx, VmFunction *vmFunction)
{
	RegVmLoweredFunction *lowFunction = new (ctx.get<RegVmLoweredFunction>()) RegVmLoweredFunction(ctx.allocator, vmFunction);

	assert(vmFunction->firstBlock);

	for(VmBlock *vmBlock = vmFunction->firstBlock; vmBlock; vmBlock = vmBlock->nextSibling)
	{
		lowFunction->blocks.push_back(RegVmLowerBlock(ctx, lowFunction, vmBlock));

		if(lowFunction->hasRegisterOverflow)
			break;
	}

	return lowFunction;
}

RegVmLoweredModule* RegVmLowerModule(ExpressionContext &ctx, VmModule *vmModule)
{
	RegVmLoweredModule *lowModule = new (ctx.get<RegVmLoweredModule>()) RegVmLoweredModule(ctx.allocator, vmModule);

	for(VmFunction *vmFunction = vmModule->functions.head; vmFunction; vmFunction = vmFunction->next)
	{
		if(vmFunction->function && vmFunction->function->importModule != NULL)
			continue;

		if(vmFunction->function && vmFunction->function->isPrototype && !vmFunction->function->implementation)
			continue;

		RegVmLoweredFunction *lowFunction = RegVmLowerFunction(ctx, vmFunction);

		lowModule->functions.push_back(lowFunction);

		if(lowFunction->hasRegisterOverflow)
			break;
	}

	return lowModule;
}

void RegFinalizeInstruction(InstructionRegVmFinalizeContext &ctx, RegVmLoweredInstruction *lowInstruction)
{
	ctx.locations.push_back(lowInstruction->location);

	RegVmCmd cmd;

	cmd.code = (unsigned char)lowInstruction->code;

	cmd.rA = lowInstruction->rA;
	cmd.rB = lowInstruction->rB;
	cmd.rC = lowInstruction->rC;

	if(VmConstant *argument = lowInstruction->argument)
	{
		if(VariableData *container = argument->container)
		{
			unsigned moduleId = container->importModule ? container->importModule->importIndex << 24 : 0;

			cmd.argument = argument->iValue + container->offset + moduleId;
		}
		else if(VmFunction *function = argument->fValue)
		{
			if(cmd.code == rviCall || cmd.code == rviFuncAddr)
			{
				FunctionData *data = function->function;

				cmd.argument = data->functionIndex;
			}
			else
			{
				assert(!"unknown instruction argument");
			}
		}
		else if(argument->bValue)
		{
			ctx.fixupPoints.push_back(InstructionRegVmFinalizeContext::FixupPoint(ctx.cmds.size(), argument->bValue));
			cmd.argument = ~0u;
		}
		else
		{
			cmd.argument = argument->iValue;
		}
	}

	ctx.cmds.push_back(cmd);
}

void RegFinalizeBlock(InstructionRegVmFinalizeContext &ctx, RegVmLoweredBlock *lowBlock)
{
	lowBlock->vmBlock->address = ctx.cmds.size();

	for(RegVmLoweredInstruction *curr = lowBlock->firstInstruction; curr; curr = curr->nextSibling)
	{
		RegFinalizeInstruction(ctx, curr);
	}
}

void RegFinalizeFunction(InstructionRegVmFinalizeContext &ctx, RegVmLoweredFunction *lowFunction)
{
	lowFunction->vmFunction->regVmAddress = ctx.cmds.size();
	lowFunction->vmFunction->regVmRegisters = lowFunction->nextRegister == 0 ? 256 : lowFunction->nextRegister;

	for(unsigned i = 0; i < lowFunction->blocks.size(); i++)
	{
		RegVmLoweredBlock *lowBlock = lowFunction->blocks[i];

		RegFinalizeBlock(ctx, lowBlock);
	}

	for(unsigned i = 0; i < ctx.fixupPoints.size(); i++)
	{
		InstructionRegVmFinalizeContext::FixupPoint &point = ctx.fixupPoints[i];

		assert(point.target);
		assert(point.target->address != ~0u);

		ctx.cmds[point.cmdIndex].argument = point.target->address;
	}

	ctx.fixupPoints.clear();

	lowFunction->vmFunction->regVmCodeSize = ctx.cmds.size() - lowFunction->vmFunction->regVmAddress;

	ctx.currentFunction = NULL;
}

void RegVmFinalizeModule(InstructionRegVmFinalizeContext &ctx, RegVmLoweredModule *lowModule)
{
	ctx.locations.push_back(NULL);
	ctx.cmds.push_back(RegVmCmd(rviJmp, 0, 0, 0, 0));

	for(unsigned i = 0; i < lowModule->functions.size(); i++)
	{
		RegVmLoweredFunction *lowFunction = lowModule->functions[i];

		if(!lowFunction->vmFunction->function)
		{
			lowModule->vmModule->regVmGlobalCodeStart = ctx.cmds.size();

			ctx.cmds[0].argument = lowModule->vmModule->regVmGlobalCodeStart;
		}

		RegFinalizeFunction(ctx, lowFunction);
	}
}
