#pragma once

#include "Array.h"
#include "Bytecode.h"
#include "InstructionTreeRegVm.h"

typedef struct DCCallVM_ DCCallVM;

class Linker;

const int REGVM_ERROR_BUFFER_SIZE = 1024;

class ExecutorRegVm
{
public:
	ExecutorRegVm(Linker* linker);
	~ExecutorRegVm();

	void	Run(unsigned functionID, const char *arguments);
	void	Stop(const char* error);

	const char*	GetResult();
	int			GetResultInt();
	double		GetResultDouble();
	long long	GetResultLong();

	const char*	GetExecError();

	char*		GetVariableData(unsigned *count);

	void		BeginCallStack();
	unsigned	GetNextAddress();

	void*		GetStackStart();
	void*		GetStackEnd();

	void	SetBreakFunction(void *context, unsigned (*callback)(void*, unsigned));
	void	ClearBreakpoints();
	bool	AddBreakpoint(unsigned instruction, bool oneHit);
	bool	RemoveBreakpoint(unsigned instruction);

	void	UpdateInstructionPointer();

private:
	void	InitExecution();

	bool	codeRunning;

	RegVmReturnType	lastResultType;
	RegVmRegister	lastResult;

	char		execError[REGVM_ERROR_BUFFER_SIZE];
	char		execResult[64];

	// Linker and linker data
	Linker		*exLinker;

	FastVector<ExternTypeInfo>	&exTypes;
	FastVector<ExternFuncInfo>	&exFunctions;
	char			*symbols;

	RegVmCmd	*codeBase;

	FastVector<char, true, true>	dataStack;

	FastVector<RegVmCallFrame>	callStack;
	unsigned	currentFrame;

	// Stack for call argument/return result data
	unsigned	*tempStackArrayBase;
	unsigned	*tempStackArrayEnd;

	// Register file
	RegVmRegister	*regFileArrayBase;
	RegVmRegister	*regFileLastTop;
	RegVmRegister	*regFileArrayEnd;

	bool		callContinue;

	DCCallVM	*dcCallVM;

	void *breakFunctionContext;
	unsigned (*breakFunction)(void*, unsigned);

	FastVector<RegVmCmd>	breakCode;

	static RegVmReturnType RunCode(ExecutorRegVm *rvm, RegVmCmd * const codeBase, RegVmCmd *instruction, unsigned finalReturn, RegVmRegister * const regFilePtr, RegVmRegister * const regFileTop, unsigned *tempStackPtr);

	bool RunExternalFunction(unsigned funcID, unsigned *callStorage);

	RegVmCmd* ExecNop(const RegVmCmd cmd, RegVmCmd * const instruction, unsigned finalReturn, RegVmRegister * const regFilePtr);
	unsigned* ExecCall(unsigned char resultReg, unsigned char resultType, unsigned functionId, RegVmCmd * const instruction, unsigned finalReturn, RegVmRegister * const regFilePtr, RegVmRegister * const regFileTop, unsigned *tempStackPtr);
	RegVmReturnType ExecReturn(const RegVmCmd cmd, RegVmCmd * const instruction, unsigned finalReturn, RegVmRegister * const regFilePtr);

	void FixupPointer(char* ptr, const ExternTypeInfo& type, bool takeSubType);
	void FixupArray(char* ptr, const ExternTypeInfo& type);
	void FixupClass(char* ptr, const ExternTypeInfo& type);
	void FixupFunction(char* ptr);
	void FixupVariable(char* ptr, const ExternTypeInfo& type);

	bool ExtendParameterStack(char* oldBase, unsigned oldSize, RegVmCmd *current);

	static const unsigned EXEC_BREAK_SIGNAL = 0;
	static const unsigned EXEC_BREAK_RETURN = 1;
	static const unsigned EXEC_BREAK_ONCE = 2;

private:
	ExecutorRegVm(const ExecutorRegVm&);
	ExecutorRegVm& operator=(const ExecutorRegVm&);
};