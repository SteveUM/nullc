#include "NULLC/nullc.h"

#include <string.h>

#include "UnitTests.h"

#if defined(_MSC_VER)
#include <Windows.h>
double myGetPreciseTime()
{
	LARGE_INTEGER freq, count;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&count);
	double temp = double(count.QuadPart) / double(freq.QuadPart);
	return temp*1000.0;
}
#endif

namespace Tests
{
	extern const char *testMatch;
}

#if !defined(SANITIZE_FUZZER)

int main(int argc, char** argv)
{
	(void)argv;

	bool verbose = false;
	bool runSpeedTests = false;
	bool testOutput = false;
	bool testTranslationSave = false;

	for(int i = 1; i < argc; i++)
	{
		if(strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0)
			verbose = true;
		else if(strcmp(argv[i], "--speed") == 0 || strcmp(argv[i], "-s") == 0)
			runSpeedTests = true;
		else if(strcmp(argv[i], "--output") == 0 || strcmp(argv[i], "-o") == 0)
			testOutput = true;
		else if(strcmp(argv[i], "--translate") == 0 || strcmp(argv[i], "-t") == 0)
			testTranslationSave = true;
	}

	return RunTests(verbose, 0, runSpeedTests, testOutput, testTranslationSave);
}

#else

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

bool testInit = false;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	char *code = new char[size + 1];
	memcpy(code, data, size);
	code[size] = 0;

	if(!testInit)
		nullcInit("Modules/");

	if(nullcCompile(code))
	{
		char exprResult[256];
		bool exprDone = false;
		if(nullcTestEvaluateExpressionTree(exprResult, 256))
		{
			exprDone = true;

			if(strchr(exprResult, '.'))
				sprintf(exprResult, "%d", int(atof(exprResult) + 0.5));
		}

		char instResult[256];
		if(nullcTestEvaluateInstructionTree(instResult, 256))
		{
			if(strchr(instResult, '.'))
				sprintf(instResult, "%d", int(atof(instResult) + 0.5));

			if(exprDone)
				assert(strcmp(exprResult, instResult) == 0);
		}
	}

	nullcClean();

	delete[] code;

	return 0;  // Non-zero return values are reserved for future use.
}

#endif
