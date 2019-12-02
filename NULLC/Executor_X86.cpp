#include "stdafx.h"

#ifdef NULLC_BUILD_X86_JIT

#include "Executor_X86.h"
#include "CodeGen_X86.h"
#include "CodeGenRegVm_X86.h"
#include "Translator_X86.h"
#include "Linker.h"
#include "Executor_Common.h"
#include "InstructionTreeRegVmLowerGraph.h"

#if !defined(NULLC_NO_RAW_EXTERNAL_CALL)
#define dcAllocMem NULLC::alloc
#define dcFreeMem  NULLC::dealloc

#include "../external/dyncall/dyncall.h"
#endif

#ifndef __linux
	#define WIN32_LEAN_AND_MEAN
	#include <Windows.h>
#else
	typedef unsigned int DWORD;
	#include <sys/mman.h>
	#ifndef PAGESIZE
		// $ sysconf()
		#define PAGESIZE 4096
	#endif
	#include <signal.h>
#endif

namespace NULLC
{
	// Parameter stack range
	//char	*stackBaseAddress;
	//char	*stackEndAddress;

	// Four global variables
	struct DataStackHeader
	{
		uintptr_t	unused1;
		uintptr_t	lastEDI;
		uintptr_t	instructionPtr;
		uintptr_t	nextElement;
	};

	DataStackHeader	*dataHead;
	//char* parameterHead;

	// Hidden pointer to the beginning of NULLC parameter stack, skipping DataStackHeader
	uintptr_t paramDataBase;

	// Binary code range in hidden pointers
	uintptr_t binCodeStart;
	uintptr_t binCodeEnd;

	// Code run result - two DWORDs for parts of result and a type flag
	//int runResult = 0;
	//int runResult2 = 0;
	//RegVmReturnType runResultType = rvrVoid;

	// Call stack is made up by a linked list, starting from last frame, this array will hold call stack in correct order
	const unsigned STACK_TRACE_DEPTH = 1024;
	unsigned stackTrace[STACK_TRACE_DEPTH];

	// Signal that call stack contains stack of execution that ended in SEH handler with a fatal exception
	volatile bool abnormalTermination;

	// Part of state that SEH handler saves for future use
	unsigned int expCodePublic;
	unsigned int expAllocCode;
	uintptr_t expEAXstate;
	uintptr_t expECXstate;
	uintptr_t expESPstate;

	ExecutorX86	*currExecutor = NULL;

#ifndef __linux

#if defined(_M_X64)
#define RegisterIp Rip
#define RegisterAx Rax
#define RegisterCx Rcx
#define RegisterSp Rsp
#define RegisterDi Rdi
#else
#define RegisterIp Eip
#define RegisterAx Eax
#define RegisterCx Ecx
#define RegisterSp Esp
#define RegisterDi Edi
#endif

	DWORD CanWeHandleSEH(unsigned int expCode, _EXCEPTION_POINTERS* expInfo)
	{
		// Check that exception happened in NULLC code (division by zero and int overflow still catched)
		bool externalCode = expInfo->ContextRecord->RegisterIp < binCodeStart || expInfo->ContextRecord->RegisterIp > binCodeEnd;
		bool managedMemoryEnd = expInfo->ExceptionRecord->ExceptionInformation[1] > paramDataBase && expInfo->ExceptionRecord->ExceptionInformation[1] < expInfo->ContextRecord->RegisterDi + paramDataBase + 64 * 1024;

		if(externalCode && (expCode == EXCEPTION_BREAKPOINT || expCode == EXCEPTION_STACK_OVERFLOW || (expCode == EXCEPTION_ACCESS_VIOLATION && !managedMemoryEnd)))
			return (DWORD)EXCEPTION_CONTINUE_SEARCH;

		// Save part of state for later use
		expEAXstate = expInfo->ContextRecord->RegisterAx;
		expECXstate = expInfo->ContextRecord->RegisterCx;
		expESPstate = expInfo->ContextRecord->RegisterSp;
		expCodePublic = expCode;
		expAllocCode = ~0u;

		if(!externalCode && *(unsigned char*)(intptr_t)expInfo->ContextRecord->RegisterIp == 0xcc)
		{
			unsigned index = ~0u;
			for(unsigned i = 0; i < currExecutor->breakInstructions.size() && index == ~0u; i++)
			{
				if((uintptr_t)currExecutor->instAddress[currExecutor->breakInstructions[i].instIndex] == expInfo->ContextRecord->RegisterIp)
					index = i;
			}
			//printf("Found at index %d\n", index);
			if(index == ~0u)
				return EXCEPTION_CONTINUE_SEARCH;
			//printf("Returning execution (%d)\n", currExecutor->breakInstructions[index].instIndex);

			uintptr_t array[2] = { expInfo->ContextRecord->RegisterIp, 0 };
			NULLC::dataHead->instructionPtr = (uintptr_t)&array[1];

			/*unsigned command = */currExecutor->breakFunction(currExecutor->breakFunctionContext, currExecutor->breakInstructions[index].instIndex);
			//printf("Returned command %d\n", command);
			*currExecutor->instAddress[currExecutor->breakInstructions[index].instIndex] = currExecutor->breakInstructions[index].oldOpcode;
			return (DWORD)EXCEPTION_CONTINUE_EXECUTION;
		}

		// Call stack should be unwind only once on top level error, since every function in external function call chain will signal an exception if there was an exception before.
		if(!NULLC::abnormalTermination)
		{
			// Create call stack
			dataHead->instructionPtr = expInfo->ContextRecord->RegisterIp;
			uintptr_t *paramData = &dataHead->nextElement;
			int count = 0;
			while(count < (STACK_TRACE_DEPTH - 1) && paramData)
			{
				stackTrace[count++] = unsigned(paramData[-1]);
				paramData = (uintptr_t*)(*paramData);
			}
			stackTrace[count] = 0;
			dataHead->nextElement = NULL;
		}

		if(expCode == EXCEPTION_INT_DIVIDE_BY_ZERO || expCode == EXCEPTION_BREAKPOINT || expCode == EXCEPTION_STACK_OVERFLOW || expCode == EXCEPTION_INT_OVERFLOW || (expCode == EXCEPTION_ACCESS_VIOLATION && expInfo->ExceptionRecord->ExceptionInformation[1] < 0x00010000))
		{
			// Save address of access violation
			if(expCode == EXCEPTION_ACCESS_VIOLATION)
				expECXstate = (unsigned int)expInfo->ExceptionRecord->ExceptionInformation[1];

			// Mark that execution terminated abnormally
			NULLC::abnormalTermination = true;

			return EXCEPTION_EXECUTE_HANDLER;
		}

		if(expCode == EXCEPTION_ACCESS_VIOLATION)
		{
			// If access violation is in some considerable boundaries out of parameter stack, extend it
			if(managedMemoryEnd)
			{
				expAllocCode = 5;

				return EXCEPTION_EXECUTE_HANDLER;
			}
		}

		return (DWORD)EXCEPTION_CONTINUE_SEARCH;
	}

	//typedef BOOL (WINAPI *PSTSG)(PULONG);
	//PSTSG pSetThreadStackGuarantee = NULL;
#else
	sigjmp_buf errorHandler;
	
	struct JmpBufData
	{
		char data[sizeof(sigjmp_buf)];
	};
	void HandleError(int signum, struct sigcontext ctx)
	{
		bool externalCode = ctx.eip < binCodeStart || ctx.eip > binCodeEnd;
		if(signum == SIGFPE)
		{
			expCodePublic = EXCEPTION_INT_DIVIDE_BY_ZERO;
			siglongjmp(errorHandler, expCodePublic);
		}
		if(signum == SIGTRAP)
		{
			expCodePublic = EXCEPTION_ARRAY_OUT_OF_BOUNDS;
			siglongjmp(errorHandler, expCodePublic);
		}
		if(signum == SIGSEGV)
		{
			if((void*)ctx.cr2 >= NULLC::stackBaseAddress && (void*)ctx.cr2 <= NULLC::stackEndAddress)
			{
				expCodePublic = EXCEPTION_ALLOCATED_STACK_OVERFLOW;

				siglongjmp(errorHandler, expCodePublic);
			}
			if(!externalCode && ctx.cr2 < 0x00010000)
			{
				expCodePublic = EXCEPTION_INVALID_POINTER;
				siglongjmp(errorHandler, expCodePublic);
			}
		}
		signal(signum, SIG_DFL);
		raise(signum);
	}

	int MemProtect(void *addr, unsigned size, int type)
	{
		char *alignedAddr = (char*)((intptr_t)((char*)addr + PAGESIZE - 1) & ~(PAGESIZE - 1)) - PAGESIZE;
		char *alignedEnd = (char*)((intptr_t)((char*)addr + size + PAGESIZE - 1) & ~(PAGESIZE - 1));

		int result = mprotect(alignedAddr, alignedEnd - alignedAddr, type);

		return result;
	}
#endif

	typedef void (*codegenCallback)(CodeGenRegVmContext &ctx, RegVmCmd);
	codegenCallback cgFuncs[rviConvertPtr + 1];

	void UpdateFunctionPointer(unsigned dest, unsigned source)
	{
		currExecutor->functionAddress[dest * 2 + 0] = currExecutor->functionAddress[source * 2 + 0];	// function address
		currExecutor->functionAddress[dest * 2 + 1] = currExecutor->functionAddress[source * 2 + 1];	// function class
		for(unsigned i = 0; i < currExecutor->oldFunctionLists.size(); i++)
		{
			if(currExecutor->oldFunctionLists[i].count < dest * 2)
				continue;
			currExecutor->oldFunctionLists[i].list[dest * 2 + 0] = currExecutor->functionAddress[source * 2 + 0];	// function address
			currExecutor->oldFunctionLists[i].list[dest * 2 + 1] = currExecutor->functionAddress[source * 2 + 1];	// function class
		}
	}
}

ExecutorX86::ExecutorX86(Linker *linker): exLinker(linker), exFunctions(linker->exFunctions), exRegVmCode(linker->exRegVmCode), exRegVmConstants(linker->exRegVmConstants), exTypes(linker->exTypes)
{
	codeGenCtx = NULL;

	memset(execError, 0, REGVM_X86_ERROR_BUFFER_SIZE);
	memset(execResult, 0, 64);

	codeRunning = false;

	lastResultType = rvrError;

	minStackSize = 1 * 1024 * 1024;

	currentFrame = 0;

	lastFinalReturn = 0;

	callContinue = true;

#if !defined(NULLC_NO_RAW_EXTERNAL_CALL)
	dcCallVM = NULL;
#endif

	breakFunctionContext = NULL;
	breakFunction = NULL;

	memset(codeLaunchHeader, 0, codeLaunchHeaderSize);
	oldCodeLaunchHeaderProtect = 0;

	binCode = NULL;
	binCodeStart = 0;
	binCodeSize = 0;
	binCodeReserved = 0;

	lastInstructionCount = 0;

	//callstackTop = NULL;

	oldJumpTargetCount = 0;
	oldFunctionSize = 0;
	oldCodeBodyProtect = 0;

	// Parameter stack must be aligned
	assert(sizeof(NULLC::DataStackHeader) % 16 == 0);

	//NULLC::stackBaseAddress = NULL;
	//NULLC::stackEndAddress = NULL;

	NULLC::currExecutor = this;

	linker->SetFunctionPointerUpdater(NULLC::UpdateFunctionPointer);

#ifdef __linux
	SetLongJmpTarget(NULLC::errorHandler);
#endif
}

ExecutorX86::~ExecutorX86()
{
	NULLC::dealloc(vmState.dataStackBase);

	NULLC::dealloc(vmState.callStackBase);

	NULLC::dealloc(vmState.tempStackArrayBase);

	NULLC::dealloc(vmState.regFileArrayBase);

#if !defined(NULLC_NO_RAW_EXTERNAL_CALL)
	if(dcCallVM)
		dcFree(dcCallVM);
#endif

	/*if(NULLC::stackBaseAddress)
	{
#ifndef __linux
		// Remove page guard, restoring old protection value
		DWORD unusedProtect;
		VirtualProtect((char*)NULLC::stackEndAddress - 8192, 4096, PAGE_READWRITE, &unusedProtect);
#else
		// Remove page guard, restoring old protection value
		NULLC::MemProtect((char*)NULLC::stackEndAddress - 8192, PAGESIZE, PROT_READ | PROT_WRITE);
#endif

		NULLC::alignedDealloc(NULLC::stackBaseAddress);
	}*/

	// Disable execution of code head and code body
#ifndef __linux
	DWORD unusedProtect;
	VirtualProtect((void*)codeLaunchHeader, codeLaunchHeaderSize, oldCodeLaunchHeaderProtect, &unusedProtect);
	if(binCode)
		VirtualProtect((void*)binCode, binCodeSize, oldCodeBodyProtect, &unusedProtect);
#else
	NULLC::MemProtect((void*)codeLaunchHeader, codeLaunchHeaderSize, PROT_READ | PROT_WRITE);
	if(binCode)
		NULLC::MemProtect((void*)binCode, binCodeSize, PROT_READ | PROT_WRITE);
#endif

	NULLC::dealloc(binCode);
	binCode = NULL;

	NULLC::currExecutor = NULL;

	for(unsigned i = 0; i < oldFunctionLists.size(); i++)
		NULLC::dealloc(oldFunctionLists[i].list);
	oldFunctionLists.clear();

	if(codeGenCtx)
		NULLC::destruct(codeGenCtx);
	codeGenCtx = NULL;

	x86ResetLabels();
}

bool ExecutorX86::Initialize()
{
	using namespace NULLC;

	cgFuncs[rviNop] = GenCodeCmdNop;
	cgFuncs[rviLoadByte] = GenCodeCmdLoadByte;
	cgFuncs[rviLoadWord] = GenCodeCmdLoadWord;
	cgFuncs[rviLoadDword] = GenCodeCmdLoadDword;
	cgFuncs[rviLoadLong] = GenCodeCmdLoadLong;
	cgFuncs[rviLoadFloat] = GenCodeCmdLoadFloat;
	cgFuncs[rviLoadDouble] = GenCodeCmdLoadDouble;
	cgFuncs[rviLoadImm] = GenCodeCmdLoadImm;
	cgFuncs[rviLoadImmLong] = GenCodeCmdLoadImmLong;
	cgFuncs[rviLoadImmDouble] = GenCodeCmdLoadImmDouble;
	cgFuncs[rviStoreByte] = GenCodeCmdStoreByte;
	cgFuncs[rviStoreWord] = GenCodeCmdStoreWord;
	cgFuncs[rviStoreDword] = GenCodeCmdStoreDword;
	cgFuncs[rviStoreLong] = GenCodeCmdStoreLong;
	cgFuncs[rviStoreFloat] = GenCodeCmdStoreFloat;
	cgFuncs[rviStoreDouble] = GenCodeCmdStoreDouble;
	cgFuncs[rviCombinedd] = GenCodeCmdCombinedd;
	cgFuncs[rviBreakupdd] = GenCodeCmdBreakupdd;
	cgFuncs[rviMov] = GenCodeCmdMov;
	cgFuncs[rviMovMult] = GenCodeCmdMovMult;
	cgFuncs[rviDtoi] = GenCodeCmdDtoi;
	cgFuncs[rviDtol] = GenCodeCmdDtol;
	cgFuncs[rviDtof] = GenCodeCmdDtof;
	cgFuncs[rviItod] = GenCodeCmdItod;
	cgFuncs[rviLtod] = GenCodeCmdLtod;
	cgFuncs[rviItol] = GenCodeCmdItol;
	cgFuncs[rviLtoi] = GenCodeCmdLtoi;
	cgFuncs[rviIndex] = GenCodeCmdIndex;
	cgFuncs[rviGetAddr] = GenCodeCmdGetAddr;
	cgFuncs[rviSetRange] = GenCodeCmdSetRange;
	cgFuncs[rviMemCopy] = GenCodeCmdMemCopy;
	cgFuncs[rviJmp] = GenCodeCmdJmp;
	cgFuncs[rviJmpz] = GenCodeCmdJmpz;
	cgFuncs[rviJmpnz] = GenCodeCmdJmpnz;
	cgFuncs[rviCall] = GenCodeCmdCall;
	cgFuncs[rviCallPtr] = GenCodeCmdCallPtr;
	cgFuncs[rviReturn] = GenCodeCmdReturn;
	cgFuncs[rviAddImm] = GenCodeCmdAddImm;
	cgFuncs[rviAdd] = GenCodeCmdAdd;
	cgFuncs[rviSub] = GenCodeCmdSub;
	cgFuncs[rviMul] = GenCodeCmdMul;
	cgFuncs[rviDiv] = GenCodeCmdDiv;
	cgFuncs[rviPow] = GenCodeCmdPow;
	cgFuncs[rviMod] = GenCodeCmdMod;
	cgFuncs[rviLess] = GenCodeCmdLess;
	cgFuncs[rviGreater] = GenCodeCmdGreater;
	cgFuncs[rviLequal] = GenCodeCmdLequal;
	cgFuncs[rviGequal] = GenCodeCmdGequal;
	cgFuncs[rviEqual] = GenCodeCmdEqual;
	cgFuncs[rviNequal] = GenCodeCmdNequal;
	cgFuncs[rviShl] = GenCodeCmdShl;
	cgFuncs[rviShr] = GenCodeCmdShr;
	cgFuncs[rviBitAnd] = GenCodeCmdBitAnd;
	cgFuncs[rviBitOr] = GenCodeCmdBitOr;
	cgFuncs[rviBitXor] = GenCodeCmdBitXor;
	cgFuncs[rviAddImml] = GenCodeCmdAddImml;
	cgFuncs[rviAddl] = GenCodeCmdAddl;
	cgFuncs[rviSubl] = GenCodeCmdSubl;
	cgFuncs[rviMull] = GenCodeCmdMull;
	cgFuncs[rviDivl] = GenCodeCmdDivl;
	cgFuncs[rviPowl] = GenCodeCmdPowl;
	cgFuncs[rviModl] = GenCodeCmdModl;
	cgFuncs[rviLessl] = GenCodeCmdLessl;
	cgFuncs[rviGreaterl] = GenCodeCmdGreaterl;
	cgFuncs[rviLequall] = GenCodeCmdLequall;
	cgFuncs[rviGequall] = GenCodeCmdGequall;
	cgFuncs[rviEquall] = GenCodeCmdEquall;
	cgFuncs[rviNequall] = GenCodeCmdNequall;
	cgFuncs[rviShll] = GenCodeCmdShll;
	cgFuncs[rviShrl] = GenCodeCmdShrl;
	cgFuncs[rviBitAndl] = GenCodeCmdBitAndl;
	cgFuncs[rviBitOrl] = GenCodeCmdBitOrl;
	cgFuncs[rviBitXorl] = GenCodeCmdBitXorl;
	cgFuncs[rviAddd] = GenCodeCmdAddd;
	cgFuncs[rviSubd] = GenCodeCmdSubd;
	cgFuncs[rviMuld] = GenCodeCmdMuld;
	cgFuncs[rviDivd] = GenCodeCmdDivd;
	cgFuncs[rviAddf] = GenCodeCmdAddf;
	cgFuncs[rviSubf] = GenCodeCmdSubf;
	cgFuncs[rviMulf] = GenCodeCmdMulf;
	cgFuncs[rviDivf] = GenCodeCmdDivf;
	cgFuncs[rviPowd] = GenCodeCmdPowd;
	cgFuncs[rviModd] = GenCodeCmdModd;
	cgFuncs[rviLessd] = GenCodeCmdLessd;
	cgFuncs[rviGreaterd] = GenCodeCmdGreaterd;
	cgFuncs[rviLequald] = GenCodeCmdLequald;
	cgFuncs[rviGequald] = GenCodeCmdGequald;
	cgFuncs[rviEquald] = GenCodeCmdEquald;
	cgFuncs[rviNequald] = GenCodeCmdNequald;
	cgFuncs[rviNeg] = GenCodeCmdNeg;
	cgFuncs[rviNegl] = GenCodeCmdNegl;
	cgFuncs[rviNegd] = GenCodeCmdNegd;
	cgFuncs[rviBitNot] = GenCodeCmdBitNot;
	cgFuncs[rviBitNotl] = GenCodeCmdBitNotl;
	cgFuncs[rviLogNot] = GenCodeCmdLogNot;
	cgFuncs[rviLogNotl] = GenCodeCmdLogNotl;
	cgFuncs[rviConvertPtr] = GenCodeCmdConvertPtr;

	/*
#ifndef __linux
	if(HMODULE hDLL = LoadLibrary("kernel32"))
		pSetThreadStackGuarantee = (PSTSG)GetProcAddress(hDLL, "SetThreadStackGuarantee");
#endif*/

	// Create code launch header
	unsigned char *pos = codeLaunchHeader;

#if defined(_M_X64)
	// Save non-volatile registers
	pos += x86PUSH(pos, rEBP);
	pos += x86PUSH(pos, rEBX);
	pos += x86PUSH(pos, rEDI);
	pos += x86PUSH(pos, rESI);
	pos += x86PUSH(pos, rESP);
	pos += x86PUSH(pos, rESP);
	// TODO: save non-volatile r12 r13 r14 and r15

	pos += x64MOV(pos, rRBX, rRDX);
	pos += x86CALL(pos, rECX);

	// Restore registers
	pos += x86POP(pos, rESP);
	pos += x86POP(pos, rESP);
	pos += x86POP(pos, rESI);
	pos += x86POP(pos, rEDI);
	pos += x86POP(pos, rEBX);
	pos += x86POP(pos, rEBP);

	pos += x86RET(pos);
#else
	// Stack will move around after we save all the registers later, copy the position into 'eax'
	pos += x86MOV(pos, rEAX, rESP);

	pos += x86MOV(pos, rEDX, sDWORD, rNONE, 0, rEAX, 0x10); // Generic stack top location
	pos += x86MOV(pos, sDWORD, rNONE, 0, rEDX, 0, rEAX);

	// Save all registers
	pos += x86PUSHAD(pos);

	pos += x86MOV(pos, rEDI, sDWORD, rNONE, 0, rEAX, 0x4); // Current variable stack size
	pos += x86MOV(pos, rEBP, 0); // Variable stack start
	pos += x86MOV(pos, rEAX, sDWORD, rNONE, 0, rEAX, 0xc); // nullc code position

	// Compute stack alignment
	pos += x86LEA(pos, rEBX, rNONE, 0, rESP, 4);
	pos += x86AND(pos, rEBX, 0xf);
	pos += x86MOV(pos, rECX, 0x10);
	pos += x86SUB(pos, rECX, rEBX);

	// Adjust the stack
	pos += x86SUB(pos, rESP, rECX);
	pos += x86PUSH(pos, rECX);

	// Go into nullc code
	pos += x86CALL(pos, rEAX);

	// Restore the stack
	pos += x86POP(pos, rECX);
	pos += x86ADD(pos, rESP, rECX);

	// Take address of the return struct (from arguments)
	pos += x86MOV(pos, rECX, sDWORD, rNONE, 0, rESP, 0x28);

	// Copy return value and type into the return struct
	pos += x86MOV(pos, sDWORD, rNONE, 0, rECX, 0, rEAX);
	pos += x86MOV(pos, sDWORD, rNONE, 0, rECX, 4, rEDX);
	pos += x86MOV(pos, sDWORD, rNONE, 0, rECX, 8, rEBX);

	// Restore registers
	pos += x86POPAD(pos);

	pos += x86RET(pos);
#endif

	assert(pos <= codeLaunchHeader + codeLaunchHeaderSize);

	// Enable execution of code head
#ifndef __linux
	VirtualProtect((void*)codeLaunchHeader, codeLaunchHeaderSize, PAGE_EXECUTE_READWRITE, (DWORD*)&oldCodeLaunchHeaderProtect);

	static RUNTIME_FUNCTION table[1] = { { 0, unsigned(pos - codeLaunchHeader), 0 } };
	RtlAddFunctionTable(table, 1, (uintptr_t)codeLaunchHeader);
#else
	NULLC::MemProtect((void*)codeLaunchHeader, codeLaunchHeaderSize, PROT_READ | PROT_EXEC);
#endif

	return true;
}
/*
bool ExecutorX86::InitStack()
{
	if(!NULLC::stackBaseAddress)
	{
		if(minStackSize < 8192)
		{
			strcpy(execError, "ERROR: stack memory range is too small");
			return false;
		}

		NULLC::stackBaseAddress = (char*)NULLC::alignedAlloc(minStackSize);
		NULLC::stackEndAddress = NULLC::stackBaseAddress + minStackSize;

#ifndef __linux
		DWORD unusedProtect;
		VirtualProtect((char*)NULLC::stackEndAddress - 8192, 4096, PAGE_NOACCESS, &unusedProtect);
#else
		NULLC::MemProtect((char*)NULLC::stackEndAddress - 8192, PAGESIZE, 0);
#endif

		NULLC::parameterHead = paramBase = (char*)NULLC::stackBaseAddress + sizeof(NULLC::DataStackHeader);
		NULLC::paramDataBase = (uintptr_t)NULLC::stackBaseAddress;
		NULLC::dataHead = (NULLC::DataStackHeader*)NULLC::stackBaseAddress;
	}

	return true;
}*/

bool ExecutorX86::InitExecution()
{
	if(!exRegVmCode.size())
	{
		strcpy(execError, "ERROR: no code to run");
		return false;
	}

	if(!vmState.callStackBase)
	{
		vmState.callStackBase = (CodeGenRegVmCallStackEntry*)NULLC::alloc(sizeof(CodeGenRegVmCallStackEntry) * 1024 * 8);
		memset(vmState.callStackBase, 0, sizeof(CodeGenRegVmCallStackEntry) * 1024 * 8);
		vmState.callStackEnd = vmState.callStackBase + 1024 * 8;
	}

	vmState.callStackTop = vmState.callStackBase;

	lastFinalReturn = 0;

	CommonSetLinker(exLinker);

	if(!vmState.dataStackBase)
	{
		vmState.dataStackBase = (char*)NULLC::alloc(sizeof(char) * minStackSize);
		memset(vmState.dataStackBase, 0, sizeof(char) * minStackSize);
		vmState.dataStackEnd = vmState.dataStackBase + minStackSize;
	}

	vmState.dataStackTop = vmState.dataStackBase + ((exLinker->globalVarSize + 0xf) & ~0xf);

	SetUnmanagableRange(vmState.dataStackBase, unsigned(vmState.dataStackEnd - vmState.dataStackBase));

	execError[0] = 0;

	callContinue = true;

	if(!vmState.tempStackArrayBase)
	{
		vmState.tempStackArrayBase = (unsigned*)NULLC::alloc(sizeof(unsigned) * 1024 * 32);
		memset(vmState.tempStackArrayBase, 0, sizeof(unsigned) * 1024 * 32);
		vmState.tempStackArrayEnd = vmState.tempStackArrayBase + 1024 * 32;
	}

	if(!vmState.regFileArrayBase)
	{
		vmState.regFileArrayBase = (RegVmRegister*)NULLC::alloc(sizeof(RegVmRegister) * 1024 * 32);
		memset(vmState.regFileArrayBase, 0, sizeof(RegVmRegister) * 1024 * 32);
		vmState.regFileArrayEnd = vmState.regFileArrayBase + 1024 * 32;
	}

	vmState.regFileLastTop = vmState.regFileArrayBase;

	vmState.instAddress = instAddress.data;
	vmState.codeLaunchHeader = codeLaunchHeader;

#if !defined(NULLC_NO_RAW_EXTERNAL_CALL)
	if(!dcCallVM)
	{
		dcCallVM = dcNewCallVM(4096);
		dcMode(dcCallVM, DC_CALL_C_DEFAULT);
	}
#endif

	/*NULLC::dataHead->lastEDI = 0;
	NULLC::dataHead->instructionPtr = 0;
	NULLC::dataHead->nextElement = 0;*/

	/*
#ifndef __linux
	if(NULLC::pSetThreadStackGuarantee)
	{
		unsigned long extraStack = 4096;
		NULLC::pSetThreadStackGuarantee(&extraStack);
	}
#endif*/

	//memset(NULLC::stackBaseAddress, 0, sizeof(NULLC::DataStackHeader));

	return true;
}

void ExecutorX86::Run(unsigned int functionID, const char *arguments)
{
	bool firstRun = !codeRunning || functionID == ~0u;

	if(firstRun)
	{
		if(!InitExecution())
			return;
	}

	codeRunning = true;

	RegVmReturnType retType = rvrVoid;

	unsigned instructionPos = exLinker->regVmOffsetToGlobalCode;

	bool errorState = false;

	// We will know that return is global if call stack size is equal to current
	unsigned prevLastFinalReturn = lastFinalReturn;
	lastFinalReturn = unsigned(vmState.callStackTop - vmState.callStackBase);

	unsigned prevDataSize = unsigned(vmState.dataStackTop - vmState.dataStackBase);

	assert(prevDataSize % 16 == 0);

	RegVmRegister *regFilePtr = vmState.regFileLastTop;
	RegVmRegister *regFileTop = regFilePtr + 256;

	unsigned *tempStackPtr = vmState.tempStackArrayBase;

	if(functionID != ~0u)
	{
		ExternFuncInfo &target = exFunctions[functionID];

		unsigned funcPos = ~0u;
		funcPos = target.regVmAddress;

		if(target.retType == ExternFuncInfo::RETURN_VOID)
			retType = rvrVoid;
		else if(target.retType == ExternFuncInfo::RETURN_INT)
			retType = rvrInt;
		else if(target.retType == ExternFuncInfo::RETURN_DOUBLE)
			retType = rvrDouble;
		else if(target.retType == ExternFuncInfo::RETURN_LONG)
			retType = rvrLong;

		if(funcPos == ~0u)
		{
			// Can't return complex types here
			if(target.retType == ExternFuncInfo::RETURN_UNKNOWN)
			{
				strcpy(execError, "ERROR: can't call external function with complex return type");
				return;
			}

			// Copy all arguments
			memcpy(tempStackPtr, arguments, target.bytesToPop);

			// Call function
			if(target.funcPtrWrap)
			{
				target.funcPtrWrap(target.funcPtrWrapTarget, (char*)tempStackPtr, (char*)tempStackPtr);

				if(!callContinue)
					errorState = true;
			}
			else
			{
#if !defined(NULLC_NO_RAW_EXTERNAL_CALL)
				RunRawExternalFunction(dcCallVM, exFunctions[functionID], exLinker->exLocals.data, exTypes.data, tempStackPtr);

				if(!callContinue)
					errorState = true;
#else
				Stop("ERROR: external raw function calls are disabled");

				errorState = true;
#endif
			}

			// This will disable NULLC code execution while leaving error check and result retrieval
			instructionPos = ~0u;
		}
		else
		{
			instructionPos = funcPos;

			unsigned argumentsSize = target.bytesToPop;

			if(unsigned(vmState.dataStackTop - vmState.dataStackBase) + argumentsSize >= unsigned(vmState.dataStackEnd - vmState.dataStackBase))
			{
				CodeGenRegVmCallStackEntry *entry = vmState.callStackTop;

				entry->instruction = instructionPos + 1;

				vmState.callStackTop++;

				instructionPos = ~0u;
				strcpy(execError, "ERROR: stack overflow");
				retType = rvrError;
			}
			else
			{
				// Copy arguments to new stack frame
				memcpy(vmState.dataStackTop, arguments, argumentsSize);

				unsigned stackSize = (target.stackSize + 0xf) & ~0xf;

				regFilePtr = vmState.regFileLastTop;
				regFileTop = regFilePtr + target.regVmRegisters;

				if(unsigned(vmState.dataStackTop - vmState.dataStackBase) + stackSize >= unsigned(vmState.dataStackEnd - vmState.dataStackBase))
				{
					CodeGenRegVmCallStackEntry *entry = vmState.callStackTop;

					entry->instruction = instructionPos + 1;

					vmState.callStackTop++;

					instructionPos = ~0u;
					strcpy(execError, "ERROR: stack overflow");
					retType = rvrError;
				}
				else
				{
					vmState.dataStackTop += stackSize;

					assert(argumentsSize <= stackSize);

					if(stackSize - argumentsSize)
						memset(vmState.dataStackBase + prevDataSize + argumentsSize, 0, stackSize - argumentsSize);

					regFilePtr[rvrrGlobals].ptrValue = uintptr_t(vmState.dataStackBase);
					regFilePtr[rvrrFrame].ptrValue = uintptr_t(vmState.dataStackBase + prevDataSize);
					regFilePtr[rvrrConstants].ptrValue = uintptr_t(exLinker->exRegVmConstants.data);
					regFilePtr[rvrrRegisters].ptrValue = uintptr_t(regFilePtr);
				}

				memset(regFilePtr + rvrrCount, 0, (regFileTop - regFilePtr - rvrrCount) * sizeof(regFilePtr[0]));
			}
		}
	}
	else
	{
		// If global code is executed, reset all global variables
		assert(unsigned(vmState.dataStackTop - vmState.dataStackBase) >= exLinker->globalVarSize);
		memset(vmState.dataStackBase, 0, exLinker->globalVarSize);

		regFilePtr[rvrrGlobals].ptrValue = uintptr_t(vmState.dataStackBase);
		regFilePtr[rvrrFrame].ptrValue = uintptr_t(vmState.dataStackBase);
		regFilePtr[rvrrConstants].ptrValue = uintptr_t(exLinker->exRegVmConstants.data);
		regFilePtr[rvrrRegisters].ptrValue = uintptr_t(regFilePtr);

		memset(regFilePtr + rvrrCount, 0, (regFileTop - regFilePtr - rvrrCount) * sizeof(regFilePtr[0]));
	}

	RegVmRegister *prevRegFileLastTop = vmState.regFileLastTop;

	vmState.regFileLastTop = regFileTop;

	RegVmReturnType resultType = retType;

	if(instructionPos != ~0u)
	{
		NULLC::abnormalTermination = false;

#ifdef __linux
		struct sigaction sa;
		struct sigaction sigFPE;
		struct sigaction sigTRAP;
		struct sigaction sigSEGV;
		if(firstRun)
		{
			sa.sa_handler = (void (*)(int))NULLC::HandleError;
			sigemptyset(&sa.sa_mask);
			sa.sa_flags = SA_RESTART;

			sigaction(SIGFPE, &sa, &sigFPE);
			sigaction(SIGTRAP, &sa, &sigTRAP);
			sigaction(SIGSEGV, &sa, &sigSEGV);
		}
		int errorCode = 0;

		NULLC::JmpBufData data;
		memcpy(data.data, NULLC::errorHandler, sizeof(sigjmp_buf));
		if(!(errorCode = sigsetjmp(NULLC::errorHandler, 1)))
		{
			unsigned savedSize = NULLC::dataHead->lastEDI;
			void *dummy = NULL;
			typedef	void (*nullcFunc)(int /*varSize*/, int* /*returnStruct*/, unsigned /*codeStart*/, void** /*genStackTop*/);
			nullcFunc gate = (nullcFunc)(uintptr_t)codeLaunchHeader;
			int returnStruct[3] = { 1, 2, 3 };
			gate(varSize, returnStruct, funcBinCodeStart, firstRun ? &genStackTop : &dummy);
			res1 = returnStruct[0];
			res2 = returnStruct[1];
			resT = returnStruct[2];
			NULLC::dataHead->lastEDI = savedSize;
		}
		else
		{
			if(errorCode == EXCEPTION_INT_DIVIDE_BY_ZERO)
				strcpy(execError, "ERROR: integer division by zero");
			else if(errorCode == EXCEPTION_FUNCTION_NO_RETURN)
				strcpy(execError, "ERROR: function didn't return a value");
			else if(errorCode == EXCEPTION_ARRAY_OUT_OF_BOUNDS)
				strcpy(execError, "ERROR: array index out of bounds");
			else if(errorCode == EXCEPTION_INVALID_FUNCTION)
				strcpy(execError, "ERROR: invalid function pointer");
			else if((errorCode & 0xff) == EXCEPTION_CONVERSION_ERROR)
				NULLC::SafeSprintf(execError, 512, "ERROR: cannot convert from %s ref to %s ref",
					&exLinker->exSymbols[exLinker->exTypes[NULLC::dataHead->unused1].offsetToName],
					&exLinker->exSymbols[exLinker->exTypes[errorCode >> 8].offsetToName]);
			else if(errorCode == EXCEPTION_ALLOCATED_STACK_OVERFLOW)
				strcpy(execError, "ERROR: allocated stack overflow");
			else if(errorCode == EXCEPTION_INVALID_POINTER)
				strcpy(execError, "ERROR: null pointer access");
			else if(errorCode == EXCEPTION_FAILED_TO_RESERVE)
				strcpy(execError, "ERROR: failed to reserve new stack memory");

			if(!NULLC::abnormalTermination && NULLC::dataHead->instructionPtr)
			{
				// Create call stack
				unsigned int *paramData = &NULLC::dataHead->nextElement;
				int count = 0;
				while((unsigned)count < (NULLC::STACK_TRACE_DEPTH - 1) && paramData)
				{
					NULLC::stackTrace[count++] = unsigned(paramData[-1]);
					paramData = (unsigned int*)(long long)(*paramData);
				}
				NULLC::stackTrace[count] = 0;
				NULLC::dataHead->nextElement = 0;
			}
			NULLC::dataHead->instructionPtr = 0;
			NULLC::abnormalTermination = true;
		}
		// Disable signal handlers only from top-level Run
		if(!wasCodeRunning)
		{
			sigaction(SIGFPE, &sigFPE, NULL);
			sigaction(SIGTRAP, &sigTRAP, NULL);
			sigaction(SIGSEGV, &sigSEGV, NULL);
		}

		memcpy(NULLC::errorHandler, data.data, sizeof(sigjmp_buf));
#else
		__try
		{
			unsigned char *codeStart = instAddress[instructionPos];

			typedef	uintptr_t (*nullcFunc)(unsigned char *codeStart, RegVmRegister *regFilePtr);
			nullcFunc gate = (nullcFunc)(uintptr_t)codeLaunchHeader;
			resultType = (RegVmReturnType)gate(codeStart, regFilePtr);
		}
		__except(NULLC::CanWeHandleSEH(GetExceptionCode(), GetExceptionInformation()))
		{
			if(NULLC::expCodePublic == EXCEPTION_INT_DIVIDE_BY_ZERO)
			{
				strcpy(execError, "ERROR: integer division by zero");
			}
			else if(NULLC::expCodePublic == EXCEPTION_INT_OVERFLOW)
			{
				strcpy(execError, "ERROR: integer overflow");
			}
			else if(NULLC::expCodePublic == EXCEPTION_BREAKPOINT && NULLC::expECXstate == 0)
			{
				strcpy(execError, "ERROR: array index out of bounds");
			}
			else if(NULLC::expCodePublic == EXCEPTION_BREAKPOINT && NULLC::expECXstate == 0xFFFFFFFF)
			{
				strcpy(execError, "ERROR: function didn't return a value");
			}
			else if(NULLC::expCodePublic == EXCEPTION_BREAKPOINT && NULLC::expECXstate == 0xDEADBEEF)
			{
				strcpy(execError, "ERROR: invalid function pointer");
			}
			else if(NULLC::expCodePublic == EXCEPTION_BREAKPOINT && NULLC::expECXstate != NULLC::expESPstate)
			{
				NULLC::SafeSprintf(execError, 512, "ERROR: cannot convert from %s ref to %s ref",
					NULLC::expEAXstate >= exLinker->exTypes.size() ? "%unknown%" : &exLinker->exSymbols[exLinker->exTypes[unsigned(NULLC::expEAXstate)].offsetToName],
					NULLC::expECXstate >= exLinker->exTypes.size() ? "%unknown%" : &exLinker->exSymbols[exLinker->exTypes[unsigned(NULLC::expECXstate)].offsetToName]);
			}
			else if(NULLC::expCodePublic == EXCEPTION_STACK_OVERFLOW)
			{
#ifndef __DMC__
				// Restore stack guard
				_resetstkoflw();
#endif

				strcpy(execError, "ERROR: stack overflow");
			}
			else if(NULLC::expCodePublic == EXCEPTION_ACCESS_VIOLATION)
			{
				if(NULLC::expAllocCode == 1)
					strcpy(execError, "ERROR: failed to commit old stack memory");
				else if(NULLC::expAllocCode == 2)
					strcpy(execError, "ERROR: failed to reserve new stack memory");
				else if(NULLC::expAllocCode == 3)
					strcpy(execError, "ERROR: failed to commit new stack memory");
				else if(NULLC::expAllocCode == 4)
					strcpy(execError, "ERROR: no more memory (512Mb maximum exceeded)");
				else if(NULLC::expAllocCode == 5)
					strcpy(execError, "ERROR: allocated stack overflow");
				else
					strcpy(execError, "ERROR: null pointer access");
			}
		}
#endif
	}

	vmState.regFileLastTop = prevRegFileLastTop;

	vmState.dataStackTop = vmState.dataStackBase + prevDataSize;

	if(resultType == rvrError)
	{
		errorState = true;
	}
	else
	{
		if(retType == rvrVoid)
			retType = resultType;
		else
			assert(retType == resultType && "expected different result");
	}

	// If there was an execution error
	if(errorState)
	{
		// Print call stack on error, when we get to the first function
		if(lastFinalReturn == 0)
		{
			char *currPos = execError + strlen(execError);
			currPos += NULLC::SafeSprintf(currPos, REGVM_X86_ERROR_BUFFER_SIZE - int(currPos - execError), "\r\nCall stack:\r\n");

			BeginCallStack();
			while(unsigned address = GetNextAddress())
				currPos += PrintStackFrame(address, currPos, REGVM_X86_ERROR_BUFFER_SIZE - int(currPos - execError), false);
		}

		lastFinalReturn = prevLastFinalReturn;

		// Ascertain that execution stops when there is a chain of nullcRunFunction
		callContinue = false;
		codeRunning = false;

		return;
	}

	lastFinalReturn = prevLastFinalReturn;

	lastResultType = retType;

	switch(lastResultType)
	{
	case rvrInt:
		lastResult.intValue = tempStackPtr[0];
		break;
	case rvrDouble:
		memcpy(&lastResult.doubleValue, tempStackPtr, sizeof(double));
		break;
	case rvrLong:
		memcpy(&lastResult.longValue, tempStackPtr, sizeof(long long));
		break;
	default:
		break;
	}

	if(lastFinalReturn == 0)
		codeRunning = false;
}

void ExecutorX86::Stop(const char* error)
{
	codeRunning = false;

	callContinue = false;
	NULLC::SafeSprintf(execError, REGVM_X86_ERROR_BUFFER_SIZE, "%s", error);
}

bool ExecutorX86::SetStackSize(unsigned bytes)
{
	if(codeRunning)
		return false;

	minStackSize = bytes;

	return true;
}

void ExecutorX86::ClearNative()
{
	memset(instList.data, 0, sizeof(x86Instruction) * instList.size());
	instList.clear();

	binCodeSize = 0;
	lastInstructionCount = 0;
	for(unsigned i = 0; i < oldFunctionLists.size(); i++)
		NULLC::dealloc(oldFunctionLists[i].list);
	oldFunctionLists.clear();

	functionAddress.clear();

	oldJumpTargetCount = 0;
	oldFunctionSize = 0;
}

bool ExecutorX86::TranslateToNative(bool enableLogFiles, OutputContext &output)
{
	//globalStartInBytecode = 0xffffffff;

	if(functionAddress.max <= exFunctions.size() * 2)
	{
		unsigned *newStorage = (unsigned*)NULLC::alloc(exFunctions.size() * 3 * sizeof(unsigned));
		if(functionAddress.count != 0)
			oldFunctionLists.push_back(FunctionListInfo(functionAddress.data, functionAddress.count));
		memcpy(newStorage, functionAddress.data, functionAddress.count * sizeof(unsigned));
		functionAddress.data = newStorage;
		functionAddress.count = exFunctions.size() * 2;
		functionAddress.max = exFunctions.size() * 3;
	}
	else
	{
		functionAddress.resize(exFunctions.size() * 2);
	}

	memset(instList.data, 0, sizeof(x86Instruction) * instList.size());
	instList.clear();
	instList.reserve(64);

	// Create new code generation context
	if(codeGenCtx)
		NULLC::destruct(codeGenCtx);

	codeGenCtx = NULLC::construct<CodeGenRegVmContext>();

	codeGenCtx->x86rvm = this;

	codeGenCtx->exFunctions = exFunctions.data;
	codeGenCtx->exTypes = exTypes.data;
	codeGenCtx->exLocals = exLinker->exLocals.data;
	codeGenCtx->exRegVmConstants = exRegVmConstants.data;
	codeGenCtx->exSymbols = exLinker->exSymbols.data;

	codeGenCtx->vmState = &vmState;

	vmState.ctx = codeGenCtx;

	//SetParamBase((unsigned int)(long long)paramBase);
	//SetFunctionList(exFunctions.data, functionAddress.data);
	//SetContinuePtr(&callContinue);

	codeGenCtx->ctx.SetLastInstruction(instList.data, instList.data);

	CommonSetLinker(exLinker);

	EMIT_OP(codeGenCtx->ctx, o_use32);

	codeJumpTargets.resize(exRegVmCode.size());
	if(codeJumpTargets.size())
		memset(&codeJumpTargets[lastInstructionCount], 0, codeJumpTargets.size() - lastInstructionCount);

	// Mirror extra global return so that jump to global return can be marked (cmdNop, because we will have some custom code)
	codeJumpTargets.push_back(false);
	for(unsigned i = oldJumpTargetCount, e = exLinker->regVmJumpTargets.size(); i != e; i++)
		codeJumpTargets[exLinker->regVmJumpTargets[i]] = true;

	// Remove cmdNop, because we don't want to generate code for it
	codeJumpTargets.pop_back();

	SetOptimizationLookBehind(codeGenCtx->ctx, false);

	unsigned int pos = lastInstructionCount;
	while(pos < exRegVmCode.size())
	{
		RegVmCmd &cmd = exRegVmCode[pos];

		unsigned int currSize = (int)(codeGenCtx->ctx.GetLastInstruction() - instList.data);
		instList.count = currSize;
		if(currSize + 64 >= instList.max)
			instList.grow(currSize + 64);

		codeGenCtx->ctx.SetLastInstruction(instList.data + currSize, instList.data);

		codeGenCtx->ctx.GetLastInstruction()->instID = pos + 1;

		if(codeJumpTargets[pos])
			SetOptimizationLookBehind(codeGenCtx->ctx,  false);

		codeGenCtx->currInstructionPos = pos;

		pos++;
		NULLC::cgFuncs[cmd.code](*codeGenCtx, cmd);

		SetOptimizationLookBehind(codeGenCtx->ctx, true);
	}

	// Add extra global return if there is none
	codeGenCtx->ctx.GetLastInstruction()->instID = pos + 1;

	EMIT_OP_REG(codeGenCtx->ctx, o_pop, rEBP);
	EMIT_OP_REG_NUM(codeGenCtx->ctx, o_mov, rEBX, ~0u);
	EMIT_OP(codeGenCtx->ctx, o_ret);

	instList.resize((int)(codeGenCtx->ctx.GetLastInstruction() - &instList[0]));

	// Once again, mirror extra global return so that jump to global return can be marked (cmdNop, because we will have some custom code)
	codeJumpTargets.push_back(false);

	if(enableLogFiles)
	{
		assert(!output.stream);
		output.stream = output.openStream("asmX86.txt");

		if(output.stream)
		{
			SaveListing(output);

			output.closeStream(output.stream);
			output.stream = NULL;
		}
	}

#ifdef NULLC_OPTIMIZE_X86
	// Second optimization pass, just feed generated instructions again

	// Set iterator at beginning
	SetLastInstruction(instList.data, instList.data);
	OptimizationLookBehind(false);
	// Now regenerate instructions
	for(unsigned int i = 0; i < instList.size(); i++)
	{
		// Skip trash
		if(instList[i].name == o_other || instList[i].name == o_none)
		{
			if(instList[i].instID && codeJumpTargets[instList[i].instID - 1])
			{
				GetLastInstruction()->instID = instList[i].instID;
				EMIT_OP(o_none);
				OptimizationLookBehind(false);
			}
			continue;
		}
		// If invalidation flag is set
		if(instList[i].instID && codeJumpTargets[instList[i].instID - 1])
			OptimizationLookBehind(false);
		GetLastInstruction()->instID = instList[i].instID;

		x86Instruction &inst = instList[i];
		if(inst.name == o_label)
		{
			EMIT_LABEL(inst.labelID, inst.argA.num);
			OptimizationLookBehind(true);
			continue;
		}
		switch(inst.argA.type)
		{
		case x86Argument::argNone:
			EMIT_OP(inst.name);
			break;
		case x86Argument::argNumber:
			EMIT_OP_NUM(inst.name, inst.argA.num);
			break;
		case x86Argument::argFPReg:
			EMIT_OP_FPUREG(inst.name, inst.argA.fpArg);
			break;
		case x86Argument::argLabel:
			EMIT_OP_LABEL(inst.name, inst.argA.labelID, inst.argB.num, inst.argB.ptrNum);
			break;
		case x86Argument::argReg:
			switch(inst.argB.type)
			{
			case x86Argument::argNone:
				EMIT_OP_REG(inst.name, inst.argA.reg);
				break;
			case x86Argument::argNumber:
				EMIT_OP_REG_NUM(inst.name, inst.argA.reg, inst.argB.num);
				break;
			case x86Argument::argReg:
				EMIT_OP_REG_REG(inst.name, inst.argA.reg, inst.argB.reg);
				break;
			case x86Argument::argPtr:
				EMIT_OP_REG_RPTR(inst.name, inst.argA.reg, inst.argB.ptrSize, inst.argB.ptrIndex, inst.argB.ptrMult, inst.argB.ptrBase, inst.argB.ptrNum);
				break;
			case x86Argument::argPtrLabel:
				EMIT_OP_REG_LABEL(inst.name, inst.argA.reg, inst.argB.labelID, inst.argB.ptrNum);
				break;
			default:
				break;
			}
			break;
		case x86Argument::argPtr:
			switch(inst.argB.type)
			{
			case x86Argument::argNone:
				EMIT_OP_RPTR(inst.name, inst.argA.ptrSize, inst.argA.ptrIndex, inst.argA.ptrMult, inst.argA.ptrBase, inst.argA.ptrNum);
				break;
			case x86Argument::argNumber:
				EMIT_OP_RPTR_NUM(inst.name, inst.argA.ptrSize, inst.argA.ptrIndex, inst.argA.ptrMult, inst.argA.ptrBase, inst.argA.ptrNum, inst.argB.num);
				break;
			case x86Argument::argReg:
				EMIT_OP_RPTR_REG(inst.name, inst.argA.ptrSize, inst.argA.ptrIndex, inst.argA.ptrMult, inst.argA.ptrBase, inst.argA.ptrNum, inst.argB.reg);
				break;
			default:
				break;
			}
			break;
		}
		OptimizationLookBehind(true);
	}
	unsigned int currSize = (int)(GetLastInstruction() - &instList[0]);
	for(unsigned int i = currSize; i < instList.size(); i++)
	{
		instList[i].name = o_other;
		instList[i].instID = 0;
	}
#endif

	if(enableLogFiles)
	{
		assert(!output.stream);
		output.stream = output.openStream("asmX86_opt.txt");

		if(output.stream)
		{
			SaveListing(output);

			output.closeStream(output.stream);
			output.stream = NULL;
		}
	}

	codeJumpTargets.pop_back();

	bool codeRelocated = false;
	if((binCodeSize + instList.size() * 6) > binCodeReserved)
	{
		unsigned int oldBinCodeReserved = binCodeReserved;
		binCodeReserved = binCodeSize + (instList.size()) * 8 + 4096;	// Average instruction size is 8 bytes.
		unsigned char *binCodeNew = (unsigned char*)NULLC::alloc(binCodeReserved);

		// Disable execution of old code body and enable execution of new code body
#ifndef __linux
		DWORD unusedProtect;
		if(binCode)
			VirtualProtect((void*)binCode, oldBinCodeReserved, oldCodeBodyProtect, (DWORD*)&unusedProtect);
		VirtualProtect((void*)binCodeNew, binCodeReserved, PAGE_EXECUTE_READWRITE, (DWORD*)&oldCodeBodyProtect);
#else
		if(binCode)
			NULLC::MemProtect((void*)binCode, oldBinCodeReserved, PROT_READ | PROT_WRITE);
		NULLC::MemProtect((void*)binCodeNew, binCodeReserved, PROT_READ | PROT_WRITE | PROT_EXEC);
#endif

		if(binCodeSize)
			memcpy(binCodeNew + 16, binCode + 16, binCodeSize);
		NULLC::dealloc(binCode);
		// If code is currently running, fix call stack (return addresses)
		if(codeRunning)
		{
			codeRelocated = true;
			// This must be an external function call
			assert(NULLC::dataHead->instructionPtr);
			
			uintptr_t *retvalpos = (uintptr_t*)NULLC::dataHead->instructionPtr - 1;
			if(*retvalpos >= NULLC::binCodeStart && *retvalpos <= NULLC::binCodeEnd)
				*retvalpos = (*retvalpos - NULLC::binCodeStart) + (uintptr_t)(binCodeNew + 16);

			uintptr_t *paramData = &NULLC::dataHead->nextElement;
			while(paramData)
			{
				uintptr_t *retvalpos = paramData - 1;
				if(*retvalpos >= NULLC::binCodeStart && *retvalpos <= NULLC::binCodeEnd)
					*retvalpos = (*retvalpos - NULLC::binCodeStart) + (uintptr_t)(binCodeNew + 16);
				paramData = (uintptr_t*)(*paramData);
			}
		}
		for(unsigned i = 0; i < instAddress.size(); i++)
			instAddress[i] = (instAddress[i] - NULLC::binCodeStart) + (uintptr_t)(binCodeNew + 16);
		binCode = binCodeNew;
		binCodeStart = (uintptr_t)(binCode + 16);
	}

	//SetBinaryCodeBase(binCode);

	NULLC::binCodeStart = binCodeStart;
	NULLC::binCodeEnd = binCodeStart + binCodeReserved;

	// Translate to x86
	unsigned char *bytecode = binCode + 16 + binCodeSize;
	unsigned char *code = bytecode + (!binCodeSize ? 0 : -7 /* we must destroy the pop ebp; mov ebx, code; ret; sequence */);

	instAddress.resize(exRegVmCode.size() + 1); // Extra instruction for global return
	memset(instAddress.data + lastInstructionCount, 0, (exRegVmCode.size() - lastInstructionCount + 1) * sizeof(instAddress[0]));

	x86ClearLabels();
	x86ReserveLabels(codeGenCtx->labelCount);

	x86Instruction *curr = &instList[0];

	for(unsigned int i = 0, e = instList.size(); i != e; i++)
	{
		x86Instruction &cmd = *curr;

		if(cmd.instID)
			instAddress[cmd.instID - 1] = code;	// Save VM instruction address in x86 bytecode

		switch(cmd.name)
		{
		case o_none:
		case o_nop:
			break;
		case o_mov:
			if(cmd.argA.type == x86Argument::argReg)
			{
				if(cmd.argB.type == x86Argument::argNumber)
					code += x86MOV(code, cmd.argA.reg, cmd.argB.num);
				else if(cmd.argB.type == x86Argument::argPtr)
					code += x86MOV(code, cmd.argA.reg, sDWORD, cmd.argB.ptrIndex, cmd.argB.ptrMult, cmd.argB.ptrBase, cmd.argB.ptrNum);
				else if(cmd.argB.type == x86Argument::argReg)
					code += x86MOV(code, cmd.argA.reg, cmd.argB.reg);
				else
					assert(!"unknown argument");
			}
			else if(cmd.argA.type == x86Argument::argPtr)
			{
				if(cmd.argB.type == x86Argument::argNumber)
					code += x86MOV(code, cmd.argA.ptrSize, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum, cmd.argB.num);
				else if(cmd.argB.type == x86Argument::argReg)
					code += x86MOV(code, cmd.argA.ptrSize, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum, cmd.argB.reg);
				else
					assert(!"unknown argument");
			}
			else
			{
				assert(!"unknown argument");
			}
			break;
		case o_movsx:
			assert(cmd.argB.type == x86Argument::argPtr);
			code += x86MOVSX(code, cmd.argA.reg, cmd.argB.ptrSize, cmd.argB.ptrIndex, cmd.argB.ptrMult, cmd.argB.ptrBase, cmd.argB.ptrNum);
			break;
		case o_push:
			if(cmd.argA.type == x86Argument::argNumber)
				code += x86PUSH(code, cmd.argA.num);
			else if(cmd.argA.type == x86Argument::argPtr)
				code += x86PUSH(code, cmd.argA.ptrSize, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum);
			else
				code += x86PUSH(code, cmd.argA.reg);
			break;
		case o_pop:
			if(cmd.argA.type == x86Argument::argPtr)
				code += x86POP(code, sDWORD, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum);
			else
				code += x86POP(code, cmd.argA.reg);
			break;
		case o_lea:
			if(cmd.argB.type == x86Argument::argPtrLabel)
			{
				code += x86LEA(code, cmd.argA.reg, cmd.argB.labelID, (unsigned int)(intptr_t)bytecode);
			}else{
				code += x86LEA(code, cmd.argA.reg, cmd.argB.ptrSize, cmd.argB.ptrIndex, cmd.argB.ptrMult, cmd.argB.ptrBase, cmd.argB.ptrNum);
			}
			break;
		case o_cdq:
			code += x86CDQ(code);
			break;
		case o_rep_movsd:
			code += x86REP_MOVSD(code);
			break;
		case o_rep_stosb:
			code += x86REP_STOSB(code);
			break;
		case o_rep_stosw:
			code += x86REP_STOSW(code);
			break;
		case o_rep_stosd:
			code += x86REP_STOSD(code);
			break;
		case o_rep_stosq:
			code += x86REP_STOSQ(code);
			break;

		case o_jmp:
			if(cmd.argA.type == x86Argument::argPtr)
				code += x86JMP(code, cmd.argA.ptrSize, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum);
			else
				code += x86JMP(code, cmd.argA.labelID, (cmd.argA.labelID & JUMP_NEAR) != 0);
			break;
		case o_ja:
			code += x86Jcc(code, cmd.argA.labelID, condA, (cmd.argA.labelID & JUMP_NEAR) != 0);
			break;
		case o_jae:
			code += x86Jcc(code, cmd.argA.labelID, condAE, (cmd.argA.labelID & JUMP_NEAR) != 0);
			break;
		case o_jb:
			code += x86Jcc(code, cmd.argA.labelID, condB, (cmd.argA.labelID & JUMP_NEAR) != 0);
			break;
		case o_jbe:
			code += x86Jcc(code, cmd.argA.labelID, condBE, (cmd.argA.labelID & JUMP_NEAR) != 0);
			break;
		case o_je:
			code += x86Jcc(code, cmd.argA.labelID, condE, (cmd.argA.labelID & JUMP_NEAR) != 0);
			break;
		case o_jg:
			code += x86Jcc(code, cmd.argA.labelID, condG, (cmd.argA.labelID & JUMP_NEAR) != 0);
			break;
		case o_jl:
			code += x86Jcc(code, cmd.argA.labelID, condL, (cmd.argA.labelID & JUMP_NEAR) != 0);
			break;
		case o_jne:
			code += x86Jcc(code, cmd.argA.labelID, condNE, (cmd.argA.labelID & JUMP_NEAR) != 0);
			break;
		case o_jnp:
			code += x86Jcc(code, cmd.argA.labelID, condNP, (cmd.argA.labelID & JUMP_NEAR) != 0);
			break;
		case o_jp:
			code += x86Jcc(code, cmd.argA.labelID, condP, (cmd.argA.labelID & JUMP_NEAR) != 0);
			break;
		case o_jge:
			code += x86Jcc(code, cmd.argA.labelID, condGE, (cmd.argA.labelID & JUMP_NEAR) != 0);
			break;
		case o_jle:
			code += x86Jcc(code, cmd.argA.labelID, condLE, (cmd.argA.labelID & JUMP_NEAR) != 0);
			break;
		case o_call:
			if(cmd.argA.type == x86Argument::argLabel)
				code += x86CALL(code, cmd.argA.labelID);
			else if(cmd.argA.type == x86Argument::argPtr)
				code += x86CALL(code, cmd.argA.ptrSize, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum);
			else
				code += x86CALL(code, cmd.argA.reg);
			break;
		case o_ret:
			code += x86RET(code);
			break;

		case o_fld:
			if(cmd.argA.type == x86Argument::argPtr)
				code += x86FLD(code, cmd.argA.ptrSize, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum);
			else
				code += x86FLD(code, (x87Reg)cmd.argA.fpArg);
			break;
		case o_fild:
			code += x86FILD(code, cmd.argA.ptrSize, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum);
			break;
		case o_fistp:
			code += x86FISTP(code, cmd.argA.ptrSize, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum);
			break;
		case o_fst:
			code += x86FST(code, cmd.argA.ptrSize, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum);
			break;
		case o_fstp:
			if(cmd.argA.type == x86Argument::argPtr)
			{
				code += x86FSTP(code, cmd.argA.ptrSize, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum);
			}else{
				code += x86FSTP(code, (x87Reg)cmd.argA.fpArg);
			}
			break;
		case o_fnstsw:
			code += x86FNSTSW(code);
			break;
		case o_fstcw:
			code += x86FSTCW(code);
			break;
		case o_fldcw:
			code += x86FLDCW(code, cmd.argA.ptrNum);
			break;

		case o_neg:
			if(cmd.argA.type == x86Argument::argPtr)
				code += x86NEG(code, sDWORD, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum);
			else
				code += x86NEG(code, cmd.argA.reg);
			break;
		case o_add:
			if(cmd.argA.type == x86Argument::argPtr)
			{
				if(cmd.argB.type == x86Argument::argReg)
					code += x86ADD(code, sDWORD, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum, cmd.argB.reg);
				else
					code += x86ADD(code, sDWORD, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum, cmd.argB.num);
			}else{
				if(cmd.argB.type == x86Argument::argPtr)
					code += x86ADD(code, cmd.argA.reg, cmd.argB.ptrSize, cmd.argB.ptrIndex, cmd.argB.ptrMult, cmd.argB.ptrBase, cmd.argB.ptrNum);
				else if(cmd.argB.type == x86Argument::argReg)
					code += x86ADD(code, cmd.argA.reg, cmd.argB.reg);
				else
					code += x86ADD(code, cmd.argA.reg, cmd.argB.num);
			}
			break;
		case o_adc:
			if(cmd.argA.type == x86Argument::argPtr)
			{
				if(cmd.argB.type == x86Argument::argReg)
					code += x86ADC(code, sDWORD, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum, cmd.argB.reg);
				else
					code += x86ADC(code, sDWORD, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum, cmd.argB.num);
			}else{
				if(cmd.argB.type == x86Argument::argReg)
					code += x86ADC(code, cmd.argA.reg, cmd.argB.reg);
				else
					code += x86ADC(code, cmd.argA.reg, cmd.argB.num);
			}
			break;
		case o_sub:
			if(cmd.argA.type == x86Argument::argPtr)
			{
				if(cmd.argB.type == x86Argument::argReg)
					code += x86SUB(code, sDWORD, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum, cmd.argB.reg);
				else
					code += x86SUB(code, sDWORD, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum, cmd.argB.num);
			}else{
				if(cmd.argB.type == x86Argument::argReg)
					code += x86SUB(code, cmd.argA.reg, cmd.argB.reg);
				else
					code += x86SUB(code, cmd.argA.reg, cmd.argB.num);
			}
			break;
		case o_sbb:
			if(cmd.argA.type == x86Argument::argPtr)
			{
				if(cmd.argB.type == x86Argument::argReg)
					code += x86SBB(code, sDWORD, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum, cmd.argB.reg);
				else
					code += x86SBB(code, sDWORD, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum, cmd.argB.num);
			}else{
				code += x86SBB(code, cmd.argA.reg, cmd.argB.num);
			}
			break;
		case o_imul:
			if(cmd.argB.type == x86Argument::argNumber)
				code += x86IMUL(code, cmd.argA.reg, cmd.argB.num);
			else if(cmd.argB.type == x86Argument::argReg)
				code += x86IMUL(code, cmd.argA.reg, cmd.argB.reg);
			else if(cmd.argB.type == x86Argument::argPtr)
				code += x86IMUL(code, cmd.argA.reg, sDWORD, cmd.argB.ptrIndex, cmd.argB.ptrMult, cmd.argB.ptrBase, cmd.argB.ptrNum);
			else
				code += x86IMUL(code, cmd.argA.reg);
			break;
		case o_idiv:
			if(cmd.argA.type == x86Argument::argPtr)
				code += x86IDIV(code, sDWORD, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum);
			else
				code += x86IDIV(code, cmd.argA.reg);
			break;
		case o_shl:
			if(cmd.argA.type == x86Argument::argPtr)
				code += x86SHL(code, sDWORD, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum, cmd.argB.num);
			else
				code += x86SHL(code, cmd.argA.reg, cmd.argB.num);
			break;
		case o_sal:
			assert(cmd.argA.type == x86Argument::argReg);
			assert(cmd.argB.reg == rECX);
			code += x86SAL(code, cmd.argA.reg);
			break;
		case o_sar:
			assert(cmd.argA.type == x86Argument::argReg);
			assert(cmd.argB.reg == rECX);
			code += x86SAR(code, cmd.argA.reg);
			break;
		case o_not:
			if(cmd.argA.type == x86Argument::argPtr)
				code += x86NOT(code, sDWORD, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum);
			else
				code += x86NOT(code, cmd.argA.reg);
			break;
		case o_and:
			if(cmd.argA.type == x86Argument::argPtr)
			{
				if(cmd.argB.type == x86Argument::argReg)
					code += x86AND(code, sDWORD, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum, cmd.argB.reg);
				else if(cmd.argB.type == x86Argument::argNumber)
					code += x86AND(code, sDWORD, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum, cmd.argB.num);
			}else{
				assert(cmd.argA.type == x86Argument::argReg);
				assert(cmd.argB.type == x86Argument::argReg || cmd.argB.type == x86Argument::argNumber);

				if(cmd.argB.type == x86Argument::argNumber)
					code += x86AND(code, cmd.argA.reg, cmd.argB.num);
				else
					code += x86AND(code, cmd.argA.reg, cmd.argB.reg);
			}
			break;
		case o_or:
			if(cmd.argA.type == x86Argument::argPtr)
			{
				if(cmd.argB.type == x86Argument::argReg)
					code += x86OR(code, sDWORD, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum, cmd.argB.reg);
				else
					code += x86OR(code, sDWORD, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum, cmd.argB.num);
			}else if(cmd.argB.type == x86Argument::argPtr){
				code += x86OR(code, cmd.argA.reg, sDWORD, cmd.argB.ptrIndex, cmd.argB.ptrMult, cmd.argB.ptrBase, cmd.argB.ptrNum);
			}else{
				assert(cmd.argA.type == x86Argument::argReg);
				assert(cmd.argB.type == x86Argument::argReg);
				code += x86OR(code, cmd.argA.reg, cmd.argB.reg);
			}
			break;
		case o_xor:
			if(cmd.argA.type == x86Argument::argPtr)
			{
				if(cmd.argB.type == x86Argument::argReg)
					code += x86XOR(code, sDWORD, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum, cmd.argB.reg);
				else
					code += x86XOR(code, sDWORD, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum, cmd.argB.num);
			}else{
				assert(cmd.argA.type == x86Argument::argReg);
				assert(cmd.argB.type == x86Argument::argReg);
				code += x86XOR(code, cmd.argA.reg, cmd.argB.reg);
			}
			break;
		case o_cmp:
			if(cmd.argA.type == x86Argument::argPtr)
			{
				if(cmd.argB.type == x86Argument::argNumber)
					code += x86CMP(code, sDWORD, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum, cmd.argB.num);
				else
					code += x86CMP(code, sDWORD, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum, cmd.argB.reg);
			}else{
				if(cmd.argB.type == x86Argument::argPtr)
					code += x86CMP(code, cmd.argA.reg, sDWORD, cmd.argB.ptrIndex, cmd.argB.ptrMult, cmd.argB.ptrBase, cmd.argB.ptrNum);
				else if(cmd.argB.type == x86Argument::argNumber)
					code += x86CMP(code, cmd.argA.reg, cmd.argB.num);
				else
					code += x86CMP(code, cmd.argA.reg, cmd.argB.reg);
			}
			break;
		case o_test:
			if(cmd.argB.type == x86Argument::argNumber)
				code += x86TESTah(code, (char)cmd.argB.num);
			else
				code += x86TEST(code, cmd.argA.reg, cmd.argB.reg);
			break;

		case o_setl:
			code += x86SETcc(code, condL, cmd.argA.reg);
			break;
		case o_setg:
			code += x86SETcc(code, condG, cmd.argA.reg);
			break;
		case o_setle:
			code += x86SETcc(code, condLE, cmd.argA.reg);
			break;
		case o_setge:
			code += x86SETcc(code, condGE, cmd.argA.reg);
			break;
		case o_sete:
			code += x86SETcc(code, condE, cmd.argA.reg);
			break;
		case o_setne:
			code += x86SETcc(code, condNE, cmd.argA.reg);
			break;
		case o_setz:
			code += x86SETcc(code, condZ, cmd.argA.reg);
			break;
		case o_setnz:
			code += x86SETcc(code, condNZ, cmd.argA.reg);
			break;

		case o_fadd:
			code += x86FADD(code, cmd.argA.ptrSize, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum);
			break;
		case o_faddp:
			code += x86FADDP(code);
			break;
		case o_fmul:
			code += x86FMUL(code, cmd.argA.ptrSize, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum);
			break;
		case o_fmulp:
			code += x86FMULP(code);
			break;
		case o_fsub:
			code += x86FSUB(code, cmd.argA.ptrSize, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum);
			break;
		case o_fsubr:
			code += x86FSUBR(code, cmd.argA.ptrSize, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum);
			break;
		case o_fsubp:
			code += x86FSUBP(code);
			break;
		case o_fsubrp:
			code += x86FSUBRP(code);
			break;
		case o_fdiv:
			code += x86FDIV(code, cmd.argA.ptrSize, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum);
			break;
		case o_fdivr:
			code += x86FDIVR(code, cmd.argA.ptrSize, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum);
			break;
		case o_fdivrp:
			code += x86FDIVRP(code);
			break;
		case o_fchs:
			code += x86FCHS(code);
			break;
		case o_fprem:
			code += x86FPREM(code);
			break;
		case o_fcomp:
			code += x86FCOMP(code, cmd.argA.ptrSize, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum);
			break;
		case o_fldz:
			code += x86FLDZ(code);
			break;
		case o_fld1:
			code += x86FLD1(code);
			break;
		case o_fsincos:
			code += x86FSINCOS(code);
			break;
		case o_fptan:
			code += x86FPTAN(code);
			break;
		case o_fsqrt:
			code += x86FSQRT(code);
			break;
		case o_frndint:
			code += x86FRNDINT(code);
			break;

		case o_movss:
			assert(cmd.argA.type == x86Argument::argPtr);
			assert(cmd.argB.type == x86Argument::argXmmReg);
			code += x86MOVSS(code, cmd.argA.ptrSize, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum, cmd.argB.xmmArg);
			break;
		case o_movsd:
			if(cmd.argA.type == x86Argument::argPtr)
			{
				assert(cmd.argB.type == x86Argument::argXmmReg);
				code += x86MOVSD(code, cmd.argA.ptrSize, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum, cmd.argB.xmmArg);
			}
			else
			{
				assert(cmd.argA.type == x86Argument::argXmmReg);
				assert(cmd.argB.type == x86Argument::argPtr);
				code += x86MOVSD(code, cmd.argA.xmmArg, cmd.argB.ptrSize, cmd.argB.ptrIndex, cmd.argB.ptrMult, cmd.argB.ptrBase, cmd.argB.ptrNum);
			}
			break;
		case o_movd:
			assert(cmd.argA.type == x86Argument::argReg);
			assert(cmd.argB.type == x86Argument::argXmmReg);
			code += x86MOVD(code, cmd.argA.reg, cmd.argB.xmmArg);
			break;
		case o_cvtss2sd:
			assert(cmd.argA.type == x86Argument::argXmmReg);
			assert(cmd.argB.type == x86Argument::argPtr);
			code += x86CVTSS2SD(code, cmd.argA.xmmArg, cmd.argB.ptrSize, cmd.argB.ptrIndex, cmd.argB.ptrMult, cmd.argB.ptrBase, cmd.argB.ptrNum);
			break;
		case o_cvtsd2ss:
			assert(cmd.argA.type == x86Argument::argXmmReg);
			assert(cmd.argB.type == x86Argument::argPtr);
			code += x86CVTSD2SS(code, cmd.argA.xmmArg, cmd.argB.ptrSize, cmd.argB.ptrIndex, cmd.argB.ptrMult, cmd.argB.ptrBase, cmd.argB.ptrNum);
			break;
		case o_cvttsd2si:
			assert(cmd.argA.type == x86Argument::argReg);
			assert(cmd.argB.type == x86Argument::argPtr);
			code += x86CVTTSD2SI(code, cmd.argA.reg, cmd.argB.ptrSize, cmd.argB.ptrIndex, cmd.argB.ptrMult, cmd.argB.ptrBase, cmd.argB.ptrNum);
			break;
		case o_cvtsi2sd:
			assert(cmd.argA.type == x86Argument::argXmmReg);
			assert(cmd.argB.type == x86Argument::argPtr);
			code += x86CVTSI2SD(code, cmd.argA.xmmArg, cmd.argB.ptrSize, cmd.argB.ptrIndex, cmd.argB.ptrMult, cmd.argB.ptrBase, cmd.argB.ptrNum);
			break;
		case o_addsd:
			assert(cmd.argA.type == x86Argument::argXmmReg);
			assert(cmd.argB.type == x86Argument::argXmmReg);
			code += x86ADDSD(code, cmd.argA.xmmArg, cmd.argB.xmmArg);
			break;
		case o_subsd:
			assert(cmd.argA.type == x86Argument::argXmmReg);
			assert(cmd.argB.type == x86Argument::argXmmReg);
			code += x86SUBSD(code, cmd.argA.xmmArg, cmd.argB.xmmArg);
			break;
		case o_mulsd:
			assert(cmd.argA.type == x86Argument::argXmmReg);
			assert(cmd.argB.type == x86Argument::argXmmReg);
			code += x86MULSD(code, cmd.argA.xmmArg, cmd.argB.xmmArg);
			break;
		case o_divsd:
			assert(cmd.argA.type == x86Argument::argXmmReg);
			assert(cmd.argB.type == x86Argument::argXmmReg);
			code += x86DIVSD(code, cmd.argA.xmmArg, cmd.argB.xmmArg);
			break;
		case o_cmpeqsd:
			assert(cmd.argA.type == x86Argument::argXmmReg);
			assert(cmd.argB.type == x86Argument::argXmmReg);
			code += x86CMPEQSD(code, cmd.argA.xmmArg, cmd.argB.xmmArg);
			break;
		case o_cmpltsd:
			assert(cmd.argA.type == x86Argument::argXmmReg);
			assert(cmd.argB.type == x86Argument::argXmmReg);
			code += x86CMPLTSD(code, cmd.argA.xmmArg, cmd.argB.xmmArg);
			break;
		case o_cmplesd:
			assert(cmd.argA.type == x86Argument::argXmmReg);
			assert(cmd.argB.type == x86Argument::argXmmReg);
			code += x86CMPLESD(code, cmd.argA.xmmArg, cmd.argB.xmmArg);
			break;
		case o_cmpneqsd:
			assert(cmd.argA.type == x86Argument::argXmmReg);
			assert(cmd.argB.type == x86Argument::argXmmReg);
			code += x86CMPNEQSD(code, cmd.argA.xmmArg, cmd.argB.xmmArg);
			break;

		case o_int:
			code += x86INT(code, 3);
			break;
		case o_label:
			x86AddLabel(code, cmd.labelID);
			break;
		case o_use32:
			break;
		case o_other:
			break;
		case o_mov64:
			if(cmd.argA.type != x86Argument::argPtr)
			{
				if(cmd.argB.type == x86Argument::argNumber)
					code += x64MOV(code, cmd.argA.reg, cmd.argB.num);
				else if(cmd.argB.type == x86Argument::argImm64)
					code += x64MOV(code, cmd.argA.reg, cmd.argB.imm64Arg);
				else if(cmd.argB.type == x86Argument::argPtr)
					code += x86MOV(code, cmd.argA.reg, cmd.argB.ptrSize, cmd.argB.ptrIndex, cmd.argB.ptrMult, cmd.argB.ptrBase, cmd.argB.ptrNum);
				else
					code += x64MOV(code, cmd.argA.reg, cmd.argB.reg);
			}
			else
			{
				if(cmd.argB.type == x86Argument::argNumber)
					code += x86MOV(code, cmd.argA.ptrSize, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum, cmd.argB.num);
				else
					code += x86MOV(code, cmd.argA.ptrSize, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum, cmd.argB.reg);
			}
			break;
		case o_movsxd:
			assert(cmd.argA.type == x86Argument::argReg);
			assert(cmd.argB.type == x86Argument::argPtr);
			code += x86MOVSXD(code, cmd.argA.reg, cmd.argB.ptrSize, cmd.argB.ptrIndex, cmd.argB.ptrMult, cmd.argB.ptrBase, cmd.argB.ptrNum);
			break;

		case o_neg64:
			assert(cmd.argA.type == x86Argument::argReg);
			code += x64NEG(code, cmd.argA.reg);
			break;
		case o_add64:
			if(cmd.argA.type == x86Argument::argPtr)
			{
				if(cmd.argB.type == x86Argument::argReg)
					code += x86ADD(code, cmd.argA.ptrSize, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum, cmd.argB.reg);
				else
					code += x86ADD(code, cmd.argA.ptrSize, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum, cmd.argB.num);
			}
			else
			{
				if(cmd.argB.type == x86Argument::argPtr)
					code += x86ADD(code, cmd.argA.reg, cmd.argB.ptrSize, cmd.argB.ptrIndex, cmd.argB.ptrMult, cmd.argB.ptrBase, cmd.argB.ptrNum);
				else if(cmd.argB.type == x86Argument::argReg)
					code += x64ADD(code, cmd.argA.reg, cmd.argB.reg);
				else
					code += x64ADD(code, cmd.argA.reg, cmd.argB.num);
			}
			break;
		case o_sub64:
			if(cmd.argA.type == x86Argument::argPtr)
			{
				if(cmd.argB.type == x86Argument::argReg)
					code += x86SUB(code, cmd.argA.ptrSize, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum, cmd.argB.reg);
				else
					code += x86SUB(code, cmd.argA.ptrSize, cmd.argA.ptrIndex, cmd.argA.ptrMult, cmd.argA.ptrBase, cmd.argA.ptrNum, cmd.argB.num);
			}
			else
			{
				if(cmd.argB.type == x86Argument::argReg)
					code += x64SUB(code, cmd.argA.reg, cmd.argB.reg);
				else
					code += x64SUB(code, cmd.argA.reg, cmd.argB.num);
			}
			break;
		case o_imul64:
			assert(cmd.argA.type == x86Argument::argReg);
			assert(cmd.argB.type == x86Argument::argReg);
			code += x64IMUL(code, cmd.argA.reg, cmd.argB.reg);
			break;
		case o_sal64:
			assert(cmd.argA.type == x86Argument::argReg);
			assert(cmd.argB.reg == rECX);
			code += x64SAL(code, cmd.argA.reg);
			break;
		case o_sar64:
			assert(cmd.argA.type == x86Argument::argReg);
			assert(cmd.argB.reg == rECX);
			code += x64SAR(code, cmd.argA.reg);
			break;
		case o_not64:
			assert(cmd.argA.type == x86Argument::argReg);
			code += x64NOT(code, cmd.argA.reg);
			break;
		case o_and64:
			assert(cmd.argA.type == x86Argument::argReg);
			assert(cmd.argB.type == x86Argument::argReg);
			code += x64AND(code, cmd.argA.reg, cmd.argB.reg);
			break;
		case o_or64:
			assert(cmd.argA.type == x86Argument::argReg);
			assert(cmd.argB.type == x86Argument::argReg);
			code += x64OR(code, cmd.argA.reg, cmd.argB.reg);
			break;
		case o_xor64:
			assert(cmd.argA.type == x86Argument::argReg);
			assert(cmd.argB.type == x86Argument::argReg);
			code += x64XOR(code, cmd.argA.reg, cmd.argB.reg);
			break;
		case o_cmp64:
			assert(cmd.argA.type == x86Argument::argReg);
			assert(cmd.argB.type == x86Argument::argReg);
			code += x64CMP(code, cmd.argA.reg, cmd.argB.reg);
			break;
		case o_cvttsd2si64:
			assert(cmd.argA.type == x86Argument::argReg);
			assert(cmd.argB.type == x86Argument::argPtr);
			code += x64CVTTSD2SI(code, cmd.argA.reg, cmd.argB.ptrSize, cmd.argB.ptrIndex, cmd.argB.ptrMult, cmd.argB.ptrBase, cmd.argB.ptrNum);
			break;
		default:
			assert(!"unknown instruction");
		}
		curr++;
	}
	assert(binCodeSize < binCodeReserved);
	binCodeSize = (unsigned int)(code - (binCode + 16));

	x86SatisfyJumps(instAddress);

	for(unsigned int i = (codeRelocated ? 0 : oldFunctionSize); i < exFunctions.size(); i++)
	{
		if(exFunctions[i].regVmAddress != -1)
		{
			exFunctions[i].startInByteCode = (int)(instAddress[exFunctions[i].regVmAddress] - (binCode + 16));

			functionAddress[i * 2 + 0] = (unsigned int)(uintptr_t)instAddress[exFunctions[i].regVmAddress];
			functionAddress[i * 2 + 1] = 0;
		}
		else if(exFunctions[i].funcPtrWrap)
		{
			exFunctions[i].startInByteCode = 0xffffffff;

			assert((uintptr_t)exFunctions[i].funcPtrWrapTarget > 1);

			functionAddress[i * 2 + 0] = (unsigned int)(uintptr_t)exFunctions[i].funcPtrWrap;
			functionAddress[i * 2 + 1] = (unsigned int)(uintptr_t)exFunctions[i].funcPtrWrapTarget;
		}
		else
		{
			exFunctions[i].startInByteCode = 0xffffffff;

			functionAddress[i * 2 + 0] = (unsigned int)(uintptr_t)exFunctions[i].funcPtrRaw;
			functionAddress[i * 2 + 1] = 1;
		}
	}
	if(codeRelocated && oldFunctionLists.size())
	{
		for(unsigned i = 0; i < oldFunctionLists.size(); i++)
			memcpy(oldFunctionLists[i].list, functionAddress.data, oldFunctionLists[i].count * sizeof(unsigned));
	}
	//globalStartInBytecode = (int)(instAddress[exLinker->regVmOffsetToGlobalCode] - (binCode + 16));

	lastInstructionCount = exRegVmCode.size();

	oldJumpTargetCount = exLinker->regVmJumpTargets.size();
	oldFunctionSize = exFunctions.size();

	return true;
}

void ExecutorX86::SaveListing(OutputContext &output)
{
	char instBuf[128];

	for(unsigned i = 0; i < instList.size(); i++)
	{
		if(instList[i].instID && codeJumpTargets[instList[i].instID - 1])
		{
			output.Print("; ------------------- Invalidation ----------------\n");
			output.Printf("0x%x: ; %4d\n", 0xc0000000 | (instList[i].instID - 1), instList[i].instID - 1);
		}

		if(instList[i].instID && instList[i].instID - 1 < exRegVmCode.size())
		{
			RegVmCmd &cmd = exRegVmCode[instList[i].instID - 1];

			output.Printf("; %4d: ", instList[i].instID - 1);

			PrintInstruction(output, (char*)exRegVmConstants.data, RegVmInstructionCode(cmd.code), cmd.rA, cmd.rB, cmd.rC, cmd.argument, NULL);

			output.Print('\n');

			if(instList[i].name == o_other)
				continue;
		}

		instList[i].Decode(instBuf);

		output.Print(instBuf);
		output.Print('\n');
	}

	output.Flush();
}

const char* ExecutorX86::GetResult()
{
	switch(lastResultType)
	{
	case rvrDouble:
		NULLC::SafeSprintf(execResult, 64, "%f", lastResult.doubleValue);
		break;
	case rvrLong:
		NULLC::SafeSprintf(execResult, 64, "%lldL", (long long)lastResult.longValue);
		break;
	case rvrInt:
		NULLC::SafeSprintf(execResult, 64, "%d", lastResult.intValue);
		break;
	case rvrVoid:
		NULLC::SafeSprintf(execResult, 64, "no return value");
		break;
	case rvrStruct:
		NULLC::SafeSprintf(execResult, 64, "complex return value");
		break;
	default:
		break;
	}

	return execResult;
}

int ExecutorX86::GetResultInt()
{
	assert(lastResultType == rvrInt);

	return lastResult.intValue;
}

double ExecutorX86::GetResultDouble()
{
	assert(lastResultType == rvrDouble);

	return lastResult.doubleValue;
}

long long ExecutorX86::GetResultLong()
{
	assert(lastResultType == rvrLong);

	return lastResult.longValue;
}

const char*	ExecutorX86::GetExecError()
{
	return execError;
}

char* ExecutorX86::GetVariableData(unsigned int *count)
{
	if(count)
		*count = unsigned(vmState.dataStackTop - vmState.dataStackBase);

	return vmState.dataStackBase;
}

void ExecutorX86::BeginCallStack()
{
	currentFrame = 0;
}

unsigned int ExecutorX86::GetNextAddress()
{
	return currentFrame == unsigned(vmState.callStackTop - vmState.callStackBase) ? 0 : vmState.callStackBase[currentFrame++].instruction;
}

void* ExecutorX86::GetStackStart()
{
	// TODO: what about temp stack?
	return vmState.regFileArrayBase;
}

void* ExecutorX86::GetStackEnd()
{
	// TODO: what about temp stack?
	return vmState.regFileLastTop;
}

void ExecutorX86::SetBreakFunction(void *context, unsigned (*callback)(void*, unsigned))
{
	breakFunctionContext = context;
	breakFunction = callback;
}

void ExecutorX86::ClearBreakpoints()
{
	for(unsigned i = 0; i < breakInstructions.size(); i++)
	{
		if(*instAddress[breakInstructions[i].instIndex] == 0xcc)
			*instAddress[breakInstructions[i].instIndex] = breakInstructions[i].oldOpcode;
	}
	breakInstructions.clear();
}

bool ExecutorX86::AddBreakpoint(unsigned int instruction, bool oneHit)
{
	if(instruction > instAddress.size())
	{
		NULLC::SafeSprintf(execError, 512, "ERROR: break position out of code range");
		return false;
	}

	while(instruction < instAddress.size() && !instAddress[instruction])
		instruction++;

	if(instruction >= instAddress.size())
	{
		NULLC::SafeSprintf(execError, 512, "ERROR: break position out of code range");
		return false;
	}

	breakInstructions.push_back(Breakpoint(instruction, *instAddress[instruction], oneHit));
	*instAddress[instruction] = 0xcc;
	return true;
}

bool ExecutorX86::RemoveBreakpoint(unsigned int instruction)
{
	if(instruction > instAddress.size())
	{
		NULLC::SafeSprintf(execError, 512, "ERROR: break position out of code range");
		return false;
	}

	unsigned index = ~0u;
	for(unsigned i = 0; i < breakInstructions.size() && index == ~0u; i++)
	{
		if(breakInstructions[i].instIndex == instruction)
			index = i;
	}

	if(index == ~0u || *instAddress[breakInstructions[index].instIndex] != 0xcc)
	{
		NULLC::SafeSprintf(execError, 512, "ERROR: there is no breakpoint at instruction %d", instruction);
		return false;
	}

	*instAddress[breakInstructions[index].instIndex] = breakInstructions[index].oldOpcode;
	return true;
}

#endif
