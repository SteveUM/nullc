#include "debug.h"

#include <thread>

#include "../../NULLC/nullc.h"
#include "../../NULLC/nullc_debug.h"

#include "context.h"
#include "handler.h"
#include "schema.h"

unsigned OnDebugBreak(void *context, unsigned instruction)
{
	Context &ctx = *(Context*)context;

	unsigned moduleIndex = 0;
	auto line = ConvertInstructionToLineAndModule(instruction, moduleIndex);

	// Skip instructions on same line
	unsigned lastAction = ctx.breakpointAction.load();

	if(lastAction == NULLC_BREAK_STEP || lastAction == NULLC_BREAK_STEP_INTO || lastAction == NULLC_BREAK_STEP_OUT)
	{
		if(ctx.breakpointLastLine.load() == line && ctx.breakpointLastModule.load() == moduleIndex)
			return lastAction;
	}

	//SendEventStopped(ctx, StoppedEventData("breakpoint", "Breakpoint Hit", 1, false, "Manual Breakpoint", true));

	ctx.breakpointActive.store(true);

	ctx.breakpointLastLine.store(line);
	ctx.breakpointLastModule.store(moduleIndex);

	if(ctx.infoMode)
		fprintf(stderr, "INFO: Breaking on instruction %d at line %d of module %d\r\n", instruction, line, moduleIndex);

	SendEventStopped(ctx, StoppedEventData("breakpoint", 1));

	{
		std::unique_lock<std::mutex> lock(ctx.breakpointMutex);

		ctx.breakpointWait.wait(lock);
	}

	ctx.breakpointActive.store(false);

	return ctx.breakpointAction.load();
}

void ApplicationThread(Context &ctx)
{
	if(nullcRunFunction(NULL))
	{
		const char *val = nullcGetResult();

		nullcFinalize();

		SendEventOutput(ctx, OutputEventData("console", val));

		ctx.running.store(false);

		SendEventThread(ctx, ThreadEventData("exited", 1));
		SendEventExited(ctx, atoi(val));
		SendEventTerminated(ctx);
	}
	else
	{
		SendEventOutput(ctx, OutputEventData("stderr", nullcGetLastError()));

		ctx.running.store(false);

		SendEventThread(ctx, ThreadEventData("exited", 1));
		SendEventExited(ctx, -1);
		SendEventTerminated(ctx);
	}
}

void LaunchApplicationThread(Context &ctx)
{
	ctx.applicationThread = std::thread([&ctx]{
		ApplicationThread(ctx);
	});
}

std::string NormalizePath(std::string path)
{
	// Lowercase and transform folder slashes in a consistent way
	for(auto &&el : path)
	{
		if(el == '\\')
			el = '/';
		else if(isalpha(el))
			el = (char)tolower(el);
	}

	return path;
}

const char* GetModuleSourceCode(Context &ctx, const Source& source)
{
	// Name is required
	if(!source.name)
		return nullptr;

	std::string name = NormalizePath(*source.name);

	auto symbols = nullcDebugSymbols(nullptr);

	unsigned moduleCount = 0;
	auto modules = nullcDebugModuleInfo(&moduleCount);

	auto fullSource = nullcDebugSource();

	for(unsigned i = 0; i < moduleCount; i++)
	{
		ExternModuleInfo &moduleInfo = modules[i];

		std::string moduleName = NormalizePath(symbols + moduleInfo.nameOffset);

		if(const char *pos = strstr(moduleName.c_str(), name.c_str()))
		{
			if(strlen(pos) == name.length())
				return fullSource + modules[i].sourceOffset;
		}
	}

	const char *mainModuleSource = fullSource + modules[moduleCount - 1].sourceOffset + modules[moduleCount - 1].sourceSize;

	std::string program = NormalizePath(*ctx.launchArgs.program);

	if(const char *pos = strstr(program.c_str(), name.c_str()))
	{
		if(strlen(pos) == name.length())
			return mainModuleSource;
	}

	return nullptr;
}

const char* GetLineStart(const char *sourceCode, int line)
{
	const char *start = sourceCode;
	int startLine = 0;

	while(*start && startLine < line)
	{
		if(*start == '\r')
		{
			start++;

			if(*start == '\n')
				start++;

			startLine++;
		}
		else if(*start == '\n')
		{
			start++;

			startLine++;
		}
		else
		{
			start++;
		}
	}

	return start;
}

const char* GetLineEnd(const char *lineStart)
{
	const char *pos = lineStart;

	while(*pos)
	{
		if(*pos == '\r')
			return pos;
		
		if(*pos == '\n')
			return pos;

		pos++;
	}

	return pos;
}

unsigned ConvertPositionToInstruction(unsigned lineStartOffset, unsigned lineEndOffset)
{
	unsigned infoSize = 0;
	auto sourceInfo = nullcDebugSourceInfo(&infoSize);

	// Find instruction
	for(unsigned i = 0; i < infoSize; i++)
	{
		if(sourceInfo[i].sourceOffset >= lineStartOffset && sourceInfo[i].sourceOffset <= lineEndOffset)
			return sourceInfo[i].instruction;
	}

	return 0;
}

unsigned ConvertLineToInstruction(const char *sourceCode, int line)
{
	const char *lineStart = GetLineStart(sourceCode, line);
	
	if(!*lineStart)
		return 0;

	const char *lineEnd = GetLineEnd(lineStart);

	auto fullSource = nullcDebugSource();

	unsigned lineStartOffset = unsigned(lineStart - fullSource);
	unsigned lineEndOffset = unsigned(lineEnd - fullSource);

	return ConvertPositionToInstruction(lineStartOffset, lineEndOffset);
}

const char* GetInstructionSourceLocation(unsigned instruction)
{
	unsigned infoSize = 0;
	auto sourceInfo = nullcDebugSourceInfo(&infoSize);

	if(!infoSize)
		return nullptr;

	auto fullSource = nullcDebugSource();

	for(unsigned i = 0; i < infoSize; i++)
	{
		if(instruction == sourceInfo[i].instruction)
			return fullSource + sourceInfo[i].sourceOffset;

		if(i + 1 < infoSize && instruction < sourceInfo[i + 1].instruction)
			return fullSource + sourceInfo[i].sourceOffset;
	}

	return fullSource + sourceInfo[infoSize - 1].sourceOffset;
}

unsigned GetSourceLocationModuleIndex(const char *sourceLocation)
{
	unsigned moduleCount = 0;
	auto modules = nullcDebugModuleInfo(&moduleCount);

	auto fullSource = nullcDebugSource();

	for(unsigned i = 0; i < moduleCount; i++)
	{
		auto &moduleInfo = modules[i];

		const char *start = fullSource + moduleInfo.sourceOffset;
		const char *end = start + moduleInfo.sourceSize;

		if(sourceLocation >= start && sourceLocation < end)
			return i;
	}

	return ~0u;
}

unsigned ConvertSourceLocationToLine(const char *sourceLocation, unsigned moduleIndex, unsigned &column)
{
	unsigned moduleCount = 0;
	auto modules = nullcDebugModuleInfo(&moduleCount);

	auto fullSource = nullcDebugSource();

	const char *sourceStart = fullSource + (moduleIndex < moduleCount ? modules[moduleIndex].sourceOffset : modules[moduleCount - 1].sourceOffset + modules[moduleCount - 1].sourceSize);

	unsigned line = 0;

	const char *pos = sourceStart;
	const char *lastLineStart = pos;

	while(pos < sourceLocation)
	{
		if(*pos == '\r')
		{
			line++;

			pos++;

			if(*pos == '\n')
				pos++;

			lastLineStart = pos;
		}
		else if(*pos == '\n')
		{
			line++;

			pos++;

			lastLineStart = pos;
		}
		else
		{
			pos++;
		}
	}

	column = int(pos - lastLineStart);

	return line;
}

unsigned ConvertInstructionToLineAndModule(unsigned instruction, unsigned &moduleIndex)
{
	auto sourceLocation = GetInstructionSourceLocation(instruction);

	moduleIndex = GetSourceLocationModuleIndex(sourceLocation);

	unsigned column = 0;
	return ConvertSourceLocationToLine(sourceLocation, moduleIndex, column);
}

std::string GetBasicVariableInfo(unsigned typeIndex, char* ptr, bool hex)
{
	char buf[1024];

	unsigned typeCount = 0;
	auto types = nullcDebugTypeInfo(&typeCount);

	auto &type = types[typeIndex];

	if(type.subCat == ExternTypeInfo::CAT_POINTER)
	{
		snprintf(buf, 256, "0x%p", *(void**)ptr);
		return buf;
	}

	if(type.subCat == ExternTypeInfo::CAT_CLASS)
	{
		if(type.type == ExternTypeInfo::TYPE_INT)
		{
			auto symbols = nullcDebugSymbols(nullptr);

			const char *memberName = symbols + type.offsetToName + (unsigned int)strlen(symbols + type.offsetToName) + 1;

			for(unsigned i = 0; i < *(unsigned*)ptr; i++)
				memberName += strlen(memberName) + 1;

			snprintf(buf, 256, hex ? "%s (0x%x)" : "%s (%d)", memberName, *(int*)ptr);
			return buf;
		}

		return "{}";
	}

	if(type.subCat == ExternTypeInfo::CAT_ARRAY)
	{
		if(type.arrSize == ~0u)
		{
			NULLCArray &arr = *(NULLCArray*)ptr;

			snprintf(buf, 256, "0x%p [%d]", arr.ptr, arr.len);
			return buf;
		}

		snprintf(buf, 256, "[%d]", type.arrSize);
		return buf;
	}

	if(type.subCat == ExternTypeInfo::CAT_FUNCTION)
	{
		unsigned functionCount = 0;
		auto functions = nullcDebugFunctionInfo(&functionCount);

		unsigned typeExtraCount = 0;
		auto typeExtras = nullcDebugTypeExtraInfo(&typeExtraCount);

		unsigned localCount = 0;
		auto locals = nullcDebugLocalInfo(&localCount);

		auto symbols = nullcDebugSymbols(nullptr);

		NULLCFuncPtr &funcPtr = *(NULLCFuncPtr*)ptr;

		auto &function = functions[funcPtr.id];
		auto &returnType = types[typeExtras[type.memberOffset].type];

		char *pos = buf;
		*pos = 0;

		pos += SafeSprintf(pos, 1024 - int(pos - buf), "%s %s(", symbols + returnType.offsetToName, symbols + function.offsetToName);

		for(unsigned i = 0; i < function.paramCount; i++)
		{
			auto &localInfo = locals[function.offsetToFirstLocal + i];

			pos += SafeSprintf(pos, 1024 - int(pos - buf), "%s %s%s", symbols + types[localInfo.type].offsetToName, symbols + localInfo.offsetToName, i == function.paramCount - 1 ? "" : ", ");
		}
		pos += SafeSprintf(pos, 1024 - int(pos - buf), ")");

		return buf;
	}

	switch(type.type)
	{
	case ExternTypeInfo::TYPE_CHAR:
		if(typeIndex == NULLC_TYPE_BOOL)
		{
			snprintf(buf, 256, *(unsigned char*)ptr ? "true" : "false");
		}
		else
		{
			if(*(char*)ptr > 0)
				snprintf(buf, 256, hex ? "'%c' (0x%x)" : "'%c' (%d)", *(char*)ptr, (int)*(char*)ptr);
			else
				snprintf(buf, 256, hex ? "0x%x" : "%d", *(char*)ptr);
		}
		break;
	case ExternTypeInfo::TYPE_SHORT:
		snprintf(buf, 256, "%d", *(short*)ptr);
		break;
	case ExternTypeInfo::TYPE_INT:
		snprintf(buf, 256, hex ? "0x%x" : "%d", *(int*)ptr);
		break;
	case ExternTypeInfo::TYPE_LONG:
		snprintf(buf, 256, hex ? "0x%llx" : "%lld", *(long long*)ptr);
		break;
	case ExternTypeInfo::TYPE_FLOAT:
		snprintf(buf, 256, "%f", *(float*)ptr);
		break;
	case ExternTypeInfo::TYPE_DOUBLE:
		snprintf(buf, 256, "%f", *(double*)ptr);
		break;
	default:
		snprintf(buf, 256, "...");
	}

	return buf;
}