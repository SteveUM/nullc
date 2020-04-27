#ifndef NULLC_DEBUG_INCLUDED
#define NULLC_DEBUG_INCLUDED

#include "nullcdef.h"
#include "Bytecode.h"

#ifdef __cplusplus
extern "C"
{
#endif

/************************************************************************/
/*							Debug functions								*/

void*				nullcGetVariableData(unsigned int *count);

unsigned int		nullcGetCurrentExecutor(void **exec);
const void*			nullcGetModule(const char* path);

/*	Used to retrieve code information of linked code	*/

ExternTypeInfo*		nullcDebugTypeInfo(unsigned int *count);
ExternMemberInfo*	nullcDebugTypeExtraInfo(unsigned int *count);
ExternVarInfo*		nullcDebugVariableInfo(unsigned int *count);
ExternFuncInfo*		nullcDebugFunctionInfo(unsigned int *count);
ExternLocalInfo*	nullcDebugLocalInfo(unsigned int *count);
char*				nullcDebugSymbols(unsigned int *count);
char*				nullcDebugSource();
ExternSourceInfo*	nullcDebugSourceInfo(unsigned int *count);
ExternModuleInfo*	nullcDebugModuleInfo(unsigned int *count);

void				nullcDebugBeginCallStack();
unsigned int		nullcDebugGetStackFrame();

#define	NULLC_BREAK_PROCEED		0
#define NULLC_BREAK_STEP		1
#define NULLC_BREAK_STEP_INTO	2
#define NULLC_BREAK_STEP_OUT	3
#define NULLC_BREAK_STOP		4

// A function that is called when breakpoint is hit. Function accepts instruction number and returns how the break should be handled (constant above)
nullres				nullcDebugSetBreakFunction(void *context, unsigned (*callback)(void*, unsigned));
// You can remove all breakpoints explicitly. nullcClean clears all breakpoints automatically
nullres				nullcDebugClearBreakpoints();
// Line number can be translated into instruction number by using nullcDebugCodeInfo and nullcDebugModuleInfo
nullres				nullcDebugAddBreakpoint(unsigned int instruction);
nullres				nullcDebugAddOneHitBreakpoint(unsigned int instruction);
nullres				nullcDebugRemoveBreakpoint(unsigned int instruction);

ExternFuncInfo*		nullcDebugConvertAddressToFunction(int instruction, ExternFuncInfo* codeFunctions, unsigned functionCount);

const char*			nullcDebugGetInstructionSourceLocation(unsigned instruction);
unsigned			nullcDebugGetSourceLocationModuleIndex(const char *sourceLocation);
unsigned			nullcDebugGetSourceLocationLineAndColumn(const char *sourceLocation, unsigned moduleIndex, unsigned &column);

NULLC_BIND unsigned				nullcDebugConvertNativeAddressToInstruction(void *address);

NULLC_BIND const char*			nullcDebugGetNativeAddressLocation(void *address);

#ifdef __cplusplus
}
#endif

#endif
