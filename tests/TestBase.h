#pragma once

#include "../NULLC/nullc.h"

#include <stdio.h>
#include <math.h>
#include <string.h>

#if !defined(_MSC_VER)
double myGetPreciseTime();
#endif

#if defined(__CELLOS_LV2__)
#	define FILE_PATH "/app_home/"
#else
#	define FILE_PATH ""
#endif

#define MODULE_PATH FILE_PATH "Modules/"

enum TestTypeIndex
{
	TEST_TYPE_VM,
	TEST_TYPE_X86,
	TEST_TYPE_LLVM,
	TEST_TYPE_EXTRA,
	TEST_TYPE_FAILURE,
	TEST_TYPE_TRANSLATION,
	TEST_TYPE_EXPR_EVALUATION,
	TEST_TYPE_INST_EVALUATION,

	TEST_TYPE_COUNT
};

#define TEST_TARGET_COUNT 3

struct TestQueue
{
	TestQueue()
	{
		this->next = 0;
		if(!head)
		{
			head = tail = this;
		}else{
			tail->next = this;
			tail = this;
		}
	}
	virtual ~TestQueue(){}

	void RunTests()
	{
		TestQueue *curr = head;
		while(curr)
		{
			curr->Run();
			curr = curr->next;
		}
	}
	virtual void Run(){}

	static TestQueue *head, *tail;
	TestQueue *next;
};

extern int testsPassed[TEST_TYPE_COUNT];
extern int testsCount[TEST_TYPE_COUNT];
extern unsigned int	testTarget[TEST_TARGET_COUNT];

struct ExternVarInfo;

namespace Tests
{
	extern bool messageVerbose;
	extern const char *lastMessage;

	extern double timeCompile;
	extern double timeGetBytecode;
	extern double timeVisit;
	extern double timeTranslate;
	extern double timeExprEvaluate;
	extern double timeInstEvaluate;
	extern double timeClean;
	extern double timeLinkCode;
	extern double timeRun;

	extern long long totalOutput;

	extern unsigned totalSyntaxNodes;
	extern unsigned totalExpressionNodes;

	extern const char		*varData;
	extern unsigned int		variableCount;
	extern ExternVarInfo	*varInfo;
	extern const char		*symbols;

	extern bool doSaveTranslation;
	extern bool doTranslation;

	extern bool	testExecutor[TEST_TARGET_COUNT];

	extern const void* (*fileLoadFunc)(const char*, unsigned int*, int*);

	extern bool enableLogFiles;
	extern void* (*openStreamFunc)(const char* name);
	extern void (*writeStreamFunc)(void *stream, const char *data, unsigned size);
	extern void (*closeStreamFunc)(void* stream);

	void*	FindVar(const char* name);
	bool	RunCode(const char *code, unsigned int executor, const char* expected, const char* message = 0, bool execShouldFail = false);
	bool	RunCodeSimple(const char *code, unsigned int executor, const char* expected, const char* message = 0, bool execShouldFail = false);
	char*	Format(const char *str, ...);
}

#define TEST_IMPL(name, code, result, count)	\
struct Test_##code : TestQueue {	\
	virtual void Run(){	\
		for(int t = 0; t < count; t++)	\
		{	\
			if(!Tests::testExecutor[t])	\
				continue;	\
			testsCount[t]++;	\
			lastFailed = false;	\
			if(!Tests::RunCode(code, t, result, name))	\
			{	\
				lastFailed = true;	\
				return;	\
			}else{	\
				RunTest();	\
			}	\
			if(!lastFailed)	\
				testsPassed[t]++;	\
		}	\
	}	\
	bool lastFailed;	\
	void RunTest();	\
};	\
Test_##code test_##code;	\
void Test_##code::RunTest()

#define TEST_IMPL_SIMPLE(name, code, result, count)	\
struct Test_##code : TestQueue {	\
	virtual void Run(){	\
		for(int t = 0; t < count; t++)	\
		{	\
			if(!Tests::testExecutor[t])	\
				continue;	\
			testsCount[t]++;	\
			lastFailed = false;	\
			if(!Tests::RunCodeSimple(code, t, result, name))	\
			{	\
				lastFailed = true;	\
				return;	\
			}else{	\
				RunTest();	\
			}	\
			if(!lastFailed)	\
				testsPassed[t]++;	\
		}	\
	}	\
	bool lastFailed;	\
	void RunTest();	\
};	\
Test_##code test_##code;	\
void Test_##code::RunTest()

#define TEST_VM(name, code, result) TEST_IMPL(name, code, result, 1)
#define TEST(name, code, result) TEST_IMPL(name, code, result, TEST_TARGET_COUNT)

#define TEST_VM_SIMPLE(name, code, result) TEST_IMPL_SIMPLE(name, code, result, 1)
#define TEST_SIMPLE(name, code, result) TEST_IMPL_SIMPLE(name, code, result, TEST_TARGET_COUNT)

#define TEST_RESULT(name, code, result)	\
struct Test_##code : TestQueue {	\
	virtual void Run(){	\
		for(int t = 0; t < TEST_TARGET_COUNT; t++)	\
		{	\
			if(!Tests::testExecutor[t])	\
				continue;	\
			testsCount[t]++;	\
			if(Tests::RunCode(code, t, result, name))	\
				testsPassed[t]++;	\
		}	\
	}	\
};	\
Test_##code test_##code;

#define TEST_RESULT_SIMPLE(name, code, result)	\
struct Test_##code : TestQueue {	\
	virtual void Run(){	\
		for(int t = 0; t < TEST_TARGET_COUNT; t++)	\
		{	\
			if(!Tests::testExecutor[t])	\
				continue;	\
			testsCount[t]++;	\
			if(Tests::RunCodeSimple(code, t, result, name))	\
				testsPassed[t]++;	\
		}	\
	}	\
};	\
Test_##code test_##code;

#define LOAD_MODULE(id, name, code)	\
struct Test_##id : TestQueue {	\
	virtual void Run(){	\
		testsCount[TEST_TYPE_EXTRA]++;	\
		if(nullcLoadModuleBySource(name, code))	\
			testsPassed[TEST_TYPE_EXTRA]++;	\
		else	\
			printf("Test " name " failed: %s\n", nullcGetLastError());	\
	}	\
};	\
Test_##id test_##id;

#define LOAD_MODULE_BIND(id, name, code)	\
struct Test_##id : TestQueue {	\
	virtual void Run(){	\
		testsCount[TEST_TYPE_EXTRA]++;	\
		if(nullcLoadModuleBySource(name, code))	\
		{	\
			testsPassed[TEST_TYPE_EXTRA]++;	\
			RunTest();	\
		}else{	\
			printf("Test " name " failed: %s\n", nullcGetLastError());	\
		}	\
	}	\
	bool lastFailed;	\
	void RunTest();	\
};	\
Test_##id test_##id;	\
void Test_##id::RunTest()

#define TEST_RELOCATE(name, code, result)	\
struct Test_##code : TestQueue {	\
	virtual void Run(){	\
		testsCount[0]++;	\
		nullcTerminate();	\
		nullcInit();	\
		nullcAddImportPath(MODULE_PATH); \
		nullcSetFileReadHandler(Tests::fileLoadFunc);	\
		nullcInitTypeinfoModule();	\
		nullcInitVectorModule();	\
		if(Tests::RunCode(code, 0, result, name))	\
			testsPassed[TEST_TYPE_VM]++;	\
	}	\
};	\
Test_##code test_##code;

#define TEST_NAME() if(Tests::lastMessage) printf("%s\r\n", Tests::lastMessage);

inline void CHECK_DOUBLE(const char *var, unsigned index, double expected, bool &lastFailed)
{
	if(void *variablePtr = Tests::FindVar(var))
	{
		double *data = (double*)variablePtr;

		if(fabs(data[index] - expected) > 1e-6)
		{
			if(Tests::lastMessage)
				printf("%s\r\n", Tests::lastMessage);

			printf(" Failed %s[%d] == %f (got %f)\r\n", var, index, (double)expected, data[index]);
			lastFailed = true;
		}
	}
	else
	{
		if(Tests::lastMessage)
			printf("%s\r\n", Tests::lastMessage);

		printf(" Failed to find variable %s\r\n", var);
		lastFailed = true;
	}
}

inline void CHECK_FLOAT(const char *var, unsigned index, float expected, bool &lastFailed)
{
	if(void *variablePtr = Tests::FindVar(var))
	{
		float *data = (float*)variablePtr;

		if (data[index] != expected)
		{
			if(Tests::lastMessage)
				printf("%s\r\n", Tests::lastMessage);

			printf(" Failed %s[%d] == %f (got %f)\r\n", var, index, (double)expected, (double)data[index]);
			lastFailed = true;
		}
	}
	else
	{
		if(Tests::lastMessage)
			printf("%s\r\n", Tests::lastMessage);

		printf(" Failed to find variable %s\r\n", var);
		lastFailed = true;
	}
}

inline void CHECK_LONG(const char *var, unsigned index, long long expected, bool &lastFailed)
{
	if(void *variablePtr = Tests::FindVar(var))
	{
		long long *data = (long long*)variablePtr;

		if (data[index] != expected)
		{
			if(Tests::lastMessage)
				printf("%s\r\n", Tests::lastMessage);

			printf(" Failed %s[%d] == %lld (got %lld)\r\n", var, index, (long long)expected, data[index]);
			lastFailed = true;
		}
	}
	else
	{
		if(Tests::lastMessage)
			printf("%s\r\n", Tests::lastMessage);

		printf(" Failed to find variable %s\r\n", var);
		lastFailed = true;
	}
}

inline void CHECK_INT(const char *var, unsigned index, int expected, bool &lastFailed)
{
	if(void *variablePtr = Tests::FindVar(var))
	{
		int *data = (int*)variablePtr;

		if (data[index] != expected)
		{
			if(Tests::lastMessage)
				printf("%s\r\n", Tests::lastMessage);

			printf(" Failed %s[%d] == %d (got %d)\r\n", var, index, expected, data[index]);
			lastFailed = true;
		}
	}
	else
	{
		if(Tests::lastMessage)
			printf("%s\r\n", Tests::lastMessage);

		printf(" Failed to find variable %s\r\n", var);
		lastFailed = true;
	}
}

inline void CHECK_SHORT(const char *var, unsigned index, short expected, bool &lastFailed)
{
	if(void *variablePtr = Tests::FindVar(var))
	{
		short *data = (short*)variablePtr;

		if (data[index] != expected)
		{
			if(Tests::lastMessage)
				printf("%s\r\n", Tests::lastMessage);

			printf(" Failed %s[%d] == %d (got %d)\r\n", var, index, expected, data[index]);
			lastFailed = true;
		}
	}
	else
	{
		if(Tests::lastMessage)
			printf("%s\r\n", Tests::lastMessage);

		printf(" Failed to find variable %s\r\n", var);
		lastFailed = true;
	}
}

inline void CHECK_CHAR(const char *var, unsigned index, char expected, bool &lastFailed)
{
	if(void *variablePtr = Tests::FindVar(var))
	{
		char *data = (char*)variablePtr;

		if (data[index] != expected)
		{
			if(Tests::lastMessage)
				printf("%s\r\n", Tests::lastMessage);

			printf(" Failed %s[%d] == %d (got %d)\r\n", var, index, expected, data[index]);
			lastFailed = true;
		}
	}
	else
	{
		if(Tests::lastMessage)
			printf("%s\r\n", Tests::lastMessage);

		printf(" Failed to find variable %s\r\n", var);
		lastFailed = true;
	}
}

inline void CHECK_STR(const char *var, unsigned index, const char* expected, bool &lastFailed)
{
	if(void *variablePtr = Tests::FindVar(var))
	{
		char *data = (char*)variablePtr + index;

		if (strcmp(data, expected) != 0)
		{
			if(Tests::lastMessage)
				printf("%s\r\n", Tests::lastMessage);

			printf(" Failed %s[%d] == %s (got %s)\r\n", var, index, expected, data);
			lastFailed = true;
		}
	}
	else
	{
		if(Tests::lastMessage)
			printf("%s\r\n", Tests::lastMessage);

		printf(" Failed to find variable %s\r\n", var);
		lastFailed = true;
	}
}

inline void CHECK_ARRAY_STR(const char *var, unsigned index, const char* expected, bool &lastFailed)
{
	if(void *variablePtr = Tests::FindVar(var))
	{
		NULLCArray *data = (NULLCArray*)variablePtr;

		if (strcmp(data[index].ptr, expected) != 0)
		{
			if(Tests::lastMessage)
				printf("%s\r\n", Tests::lastMessage);

			printf(" Failed %s[%d] == %s (got %s)\r\n", var, index, expected, data[index].ptr);
			lastFailed = true;
		}
	}
	else
	{
		if(Tests::lastMessage)
			printf("%s\r\n", Tests::lastMessage);

		printf(" Failed to find variable %s\r\n", var);
		lastFailed = true;
	}
}

inline void CHECK_HEAP_STR(const char *var, unsigned index, const char* expected, bool &lastFailed)
{
	if(void *variablePtr = Tests::FindVar(var))
	{
		char *data = *(char**)variablePtr + index;

		if (strcmp(data, expected) != 0)
		{
			if(Tests::lastMessage)
				printf("%s\r\n", Tests::lastMessage);

			printf(" Failed %s[%d] == %s (got %s)\r\n", var, index, expected, data);
			lastFailed = true;
		}
	}
	else
	{
		if(Tests::lastMessage)
			printf("%s\r\n", Tests::lastMessage);

		printf(" Failed to find variable %s\r\n", var);
		lastFailed = true;
	}
}

#if defined(NDEBUG)
#define TEST_RUNTIME_FAIL_EXECUTORS 2
#else
#define TEST_RUNTIME_FAIL_EXECUTORS 1
#endif

#define TEST_RUNTIME_FAIL(name, code, result)	\
struct Test_##code : TestQueue {	\
	virtual void Run(){	\
		for(int t = 0; t < TEST_RUNTIME_FAIL_EXECUTORS; t++)	\
		{	\
			if(!Tests::testExecutor[t])	\
				continue;	\
			testsCount[t]++;	\
			if(Tests::RunCode(code, t, result, name, true))	\
				testsPassed[t]++;	\
		}	\
	}	\
};	\
Test_##code test_##code;

void TEST_FOR_FAIL(const char* name, const char* str, const char* error);
void TEST_FOR_FAIL_GENERIC(const char* name, const char* str, const char* error1, const char* error2);
