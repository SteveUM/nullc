#include "runtime.h"

struct Zomg{ char a; int b; };

Zomg Call_Zomg_ref_Zomg_(Zomg x, void* __context)
{
	x.a = x.a == -1 && x.b == -2;
	return x;
}
