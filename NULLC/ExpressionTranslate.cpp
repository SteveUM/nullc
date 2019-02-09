#include "ExpressionTranslate.h"

#include <stdarg.h>

#include "Bytecode.h"
#include "BinaryCache.h"
#include "Compiler.h"
#include "ExpressionTree.h"

#define FMT_ISTR(x) unsigned(x.end - x.begin), x.begin

void PrintBuffered(ExpressionTranslateContext &ctx, char ch)
{
	ctx.outBuf[ctx.outBufPos++] = ch;

	if(ctx.outBufPos == ctx.outBufSize)
	{
		fwrite(ctx.outBuf, 1, ctx.outBufPos, ctx.file);
		ctx.outBufPos = 0;
	}
}

void PrintBuffered(ExpressionTranslateContext &ctx, const char *str)
{
	unsigned length = unsigned(strlen(str));

	if(ctx.outBufPos + length < ctx.outBufSize)
	{
		memcpy(ctx.outBuf + ctx.outBufPos, str, length);
		ctx.outBufPos += length;
	}
	else
	{
		unsigned remainder = ctx.outBufSize - ctx.outBufPos;

		memcpy(ctx.outBuf + ctx.outBufPos, str, remainder);
		ctx.outBufPos += remainder;

		fwrite(ctx.outBuf, 1, ctx.outBufPos, ctx.file);
		ctx.outBufPos = 0;

		str += remainder;
		length -= remainder;

		if(!length)
			return;

		if(length < ctx.outBufSize)
		{
			memcpy(ctx.outBuf + ctx.outBufPos, str, length);
			ctx.outBufPos += length;
		}
		else
		{
			fwrite(str, 1, length, ctx.file);
		}
	}
}

void PrintBuffered(ExpressionTranslateContext &ctx, const char *format, va_list args)
{
	const int tmpSize = 1024;
	char tmp[tmpSize];

	int length = vsnprintf(tmp, tmpSize - 1, format, args);

	if(length < 0 || length > tmpSize - 1)
	{
		fwrite(ctx.outBuf, 1, ctx.outBufPos, ctx.file);
		ctx.outBufPos = 0;

		vfprintf(ctx.file, format, args);
	}
	else
	{
		PrintBuffered(ctx, tmp);
	}
}

void FlushBuffered(ExpressionTranslateContext &ctx)
{
	if(ctx.outBufPos)
		fwrite(ctx.outBuf, 1, ctx.outBufPos, ctx.file);
	ctx.outBufPos = 0;
}

void Print(ExpressionTranslateContext &ctx, const char *format, ...)
{
	va_list args;
	va_start(args, format);

	PrintBuffered(ctx, format, args);

	va_end(args);
}

void PrintIndent(ExpressionTranslateContext &ctx)
{
	for(unsigned i = 0; i < ctx.depth; i++)
		PrintBuffered(ctx, ctx.indent);
}

void PrintLine(ExpressionTranslateContext &ctx)
{
	PrintBuffered(ctx, "\n");
}

void PrintIndentedLine(ExpressionTranslateContext &ctx, const char *format, ...)
{
	PrintIndent(ctx);

	va_list args;
	va_start(args, format);

	PrintBuffered(ctx, format, args);

	va_end(args);

	PrintLine(ctx);
}

void PrintEscapedName(ExpressionTranslateContext &ctx, InplaceStr name)
{
	for(unsigned i = 0; i < name.length(); i++)
	{
		char ch = name.begin[i];

		if(ch == ' ' || ch == '[' || ch == ']' || ch == '(' || ch == ')' || ch == ',' || ch == ':' || ch == '$' || ch == '<' || ch == '>' || ch == '.')
			PrintBuffered(ctx, "_");
		else
			PrintBuffered(ctx, ch);
	}
}

bool UseNonStaticTemplate(ExpressionTranslateContext &ctx, FunctionData *function)
{
	if(function->scope != ctx.ctx.globalScope && !function->scope->ownerNamespace && !function->scope->ownerType)
		return false;
	else if(*function->name.begin == '$')
		return false;
	else if(function->isHidden)
		return false;
	else if(ctx.ctx.IsGenericInstance(function))
		return true;

	return false;
}

void TranslateTypeName(ExpressionTranslateContext &ctx, TypeBase *type)
{
	if(TypeVoid *typeVoid = getType<TypeVoid>(type))
	{
		Print(ctx, "void");
	}
	else if(TypeBool *typeBool = getType<TypeBool>(type))
	{
		Print(ctx, "bool");
	}
	else if(TypeChar *typeChar = getType<TypeChar>(type))
	{
		Print(ctx, "char");
	}
	else if(TypeShort *typeShort = getType<TypeShort>(type))
	{
		Print(ctx, "short");
	}
	else if(TypeInt *typeInt = getType<TypeInt>(type))
	{
		Print(ctx, "int");
	}
	else if(TypeLong *typeLong = getType<TypeLong>(type))
	{
		Print(ctx, "long long");
	}
	else if(TypeFloat *typeFloat = getType<TypeFloat>(type))
	{
		Print(ctx, "float");
	}
	else if(TypeDouble *typeDouble = getType<TypeDouble>(type))
	{
		Print(ctx, "double");
	}
	else if(TypeTypeID *typeTypeid = getType<TypeTypeID>(type))
	{
		Print(ctx, "unsigned");
	}
	else if(TypeFunctionID *typeFunctionID = getType<TypeFunctionID>(type))
	{
		Print(ctx, "__function");
	}
	else if(TypeNullptr *typeNullptr = getType<TypeNullptr>(type))
	{
		Print(ctx, "__nullptr");
	}
	else if(TypeGeneric *typeGeneric = getType<TypeGeneric>(type))
	{
		assert(!"generic type TypeGeneric is not translated");
	}
	else if(TypeGenericAlias *typeGenericAlias = getType<TypeGenericAlias>(type))
	{
		assert(!"generic type TypeGenericAlias is not translated");
	}
	else if(TypeAuto *typeAuto = getType<TypeAuto>(type))
	{
		assert(!"virtual type TypeAuto is not translated");
	}
	else if(TypeAutoRef *typeAutoRef = getType<TypeAutoRef>(type))
	{
		Print(ctx, "NULLCRef");
	}
	else if(TypeAutoArray *typeAutoArray = getType<TypeAutoArray>(type))
	{
		Print(ctx, "NULLCAutoArray");
	}
	else if(TypeRef *typeRef = getType<TypeRef>(type))
	{
		TranslateTypeName(ctx, typeRef->subType);
		Print(ctx, "*");
	}
	else if(TypeArray *typeArray = getType<TypeArray>(type))
	{
		PrintEscapedName(ctx, typeArray->name);
	}
	else if(TypeUnsizedArray *typeUnsizedArray = getType<TypeUnsizedArray>(type))
	{
		Print(ctx, "NULLCArray< ");
		TranslateTypeName(ctx, typeUnsizedArray->subType);
		Print(ctx, " >");
	}
	else if(TypeFunction *typeFunction = getType<TypeFunction>(type))
	{
		Print(ctx, "NULLCFuncPtr<__typeProxy_");
		PrintEscapedName(ctx, typeFunction->name);
		Print(ctx, ">");
	}
	else if(TypeGenericClassProto *typeGenericClassProto = getType<TypeGenericClassProto>(type))
	{
		assert(!"generic type TypeGenericClassProto is not translated");
	}
	else if(TypeGenericClass *typeGenericClass = getType<TypeGenericClass>(type))
	{
		assert(!"generic type TypeGenericClass is not translated");
	}
	else if(TypeClass *typeClass = getType<TypeClass>(type))
	{
		PrintEscapedName(ctx, typeClass->name);
	}
	else if(TypeEnum *typeEnum = getType<TypeEnum>(type))
	{
		PrintEscapedName(ctx, typeEnum->name);
	}
	else if(TypeFunctionSet *typeFunctionSet = getType<TypeFunctionSet>(type))
	{
		assert(!"virtual type TypeFunctionSet is not translated");
	}
	else if(TypeArgumentSet *typeArgumentSet = getType<TypeArgumentSet>(type))
	{
		assert(!"virtual type TypeArgumentSet is not translated");
	}
	else if(TypeMemberSet *typeMemberSet = getType<TypeMemberSet>(type))
	{
		assert(!"virtual type TypeMemberSet is not translated");
	}
	else
	{
		assert(!"unknown type");
	}
}

void TranslateVariableName(ExpressionTranslateContext &ctx, VariableData *variable)
{
	if(variable->name == InplaceStr("this"))
	{
		Print(ctx, "__context");
	}
	else
	{
		if(*variable->name.begin == '$')
		{
			Print(ctx, "__");

			PrintEscapedName(ctx, InplaceStr(variable->name.begin + 1, variable->name.end));
		}
		else
		{
			PrintEscapedName(ctx, variable->name);
		}

		if(variable->scope != ctx.ctx.globalScope && !variable->scope->ownerType && !variable->scope->ownerFunction && !variable->scope->ownerNamespace)
			Print(ctx, "_%d", variable->uniqueId);
	}
}

void TranslateTypeDefinition(ExpressionTranslateContext &ctx, TypeBase *type)
{
	if(type->hasTranslation)
		return;

	type->hasTranslation = true;

	if(type->isGeneric)
		return;

	if(TypeFunction *typeFunction = getType<TypeFunction>(type))
	{
		Print(ctx, "struct __typeProxy_");
		PrintEscapedName(ctx, typeFunction->name);
		Print(ctx, "{};");
		PrintLine(ctx);
	}
	else if(TypeArray *typeArray = getType<TypeArray>(type))
	{
		TranslateTypeDefinition(ctx, typeArray->subType);

		Print(ctx, "struct ");
		PrintEscapedName(ctx, typeArray->name);
		PrintLine(ctx);

		PrintIndentedLine(ctx, "{");
		ctx.depth++;

		PrintIndent(ctx);
		TranslateTypeName(ctx, typeArray->subType);
		Print(ctx, " ptr[%d];", typeArray->length + ((4 - (typeArray->length * typeArray->subType->size % 4)) & 3)); // Round total byte size to 4
		PrintLine(ctx);

		PrintIndent(ctx);
		PrintEscapedName(ctx, typeArray->name);
		Print(ctx, "& set(unsigned index, ");
		TranslateTypeName(ctx, typeArray->subType);
		Print(ctx, " const& val){ ptr[index] = val; return *this; }");
		PrintLine(ctx);

		PrintIndent(ctx);
		TranslateTypeName(ctx, typeArray->subType);
		Print(ctx, "* index(unsigned i){ if(unsigned(i) < %u) return &ptr[i]; nullcThrowError(\"ERROR: array index out of bounds\"); return 0; }", (unsigned)typeArray->length);
		PrintLine(ctx);

		ctx.depth--;
		PrintIndentedLine(ctx, "};");
	}
	else if(TypeClass *typeClass = getType<TypeClass>(type))
	{
		for(VariableHandle *curr = typeClass->members.head; curr; curr = curr->next)
			TranslateTypeDefinition(ctx, curr->variable->type);

		Print(ctx, "struct ");
		PrintEscapedName(ctx, typeClass->name);
		PrintLine(ctx);

		PrintIndentedLine(ctx, "{");
		ctx.depth++;

		unsigned offset = 0;
		unsigned index = 0;

		for(VariableHandle *curr = typeClass->members.head; curr; curr = curr->next)
		{
			if(curr->variable->offset > offset)
				PrintIndentedLine(ctx, "char pad_%d[%d];", index, int(curr->variable->offset - offset));

			PrintIndent(ctx);

			TranslateTypeName(ctx, curr->variable->type);
			Print(ctx, " ");
			TranslateVariableName(ctx, curr->variable);
			Print(ctx, ";");
			PrintLine(ctx);

			offset = unsigned(curr->variable->offset + curr->variable->type->size);
			index++;
		}

		if(typeClass->padding != 0)
			PrintIndentedLine(ctx, "char pad_%d[%d];", index, typeClass->padding);

		ctx.depth--;
		PrintIndentedLine(ctx, "};");
	}
	else if(TypeEnum *typeEnum = getType<TypeEnum>(type))
	{
		Print(ctx, "struct ");
		PrintEscapedName(ctx, typeEnum->name);
		PrintLine(ctx);

		PrintIndentedLine(ctx, "{");
		ctx.depth++;

		PrintIndent(ctx);
		PrintEscapedName(ctx, typeEnum->name);
		Print(ctx, "(): value(0){}");
		PrintLine(ctx);

		PrintIndent(ctx);
		PrintEscapedName(ctx, typeEnum->name);
		Print(ctx, "(int v): value(v){}");
		PrintLine(ctx);

		PrintIndentedLine(ctx, "int value;");

		ctx.depth--;
		PrintIndentedLine(ctx, "};");
	}
}

void TranslateFunctionName(ExpressionTranslateContext &ctx, FunctionData *function)
{
	InplaceStr name = function->name;

	if(function->implementation)
		function = function->implementation;

	if(*name.begin == '$')
	{
		Print(ctx, "__");

		name = InplaceStr(name.begin + 1, name.end);
	}

	InplaceStr operatorName = GetOperatorName(name);

	if(!operatorName.empty())
		name = operatorName;

	if(function->scope->ownerType)
	{
		if(name.length() > function->scope->ownerType->name.length() + 2)
		{
			InplaceStr operatorName = GetOperatorName(InplaceStr(name.begin + function->scope->ownerType->name.length() + 2, name.end));

			if(!operatorName.empty())
			{
				PrintEscapedName(ctx, function->scope->ownerType->name);
				Print(ctx, "__");
				PrintEscapedName(ctx, operatorName);
			}
			else
			{
				PrintEscapedName(ctx, name);
			}
		}
		else
		{
			PrintEscapedName(ctx, name);
		}

		for(MatchData *alias = function->generics.head; alias; alias = alias->next)
		{
			Print(ctx, "_");
			PrintEscapedName(ctx, alias->type->name);
			Print(ctx, "_");
		}

		Print(ctx, "_");
		PrintEscapedName(ctx, function->type->name);
	}
	else if((function->scope == ctx.ctx.globalScope || function->scope->ownerNamespace) && !function->isHidden)
	{
		PrintEscapedName(ctx, name);

		for(MatchData *alias = function->generics.head; alias; alias = alias->next)
		{
			Print(ctx, "_");
			PrintEscapedName(ctx, alias->type->name);
			Print(ctx, "_");
		}

		Print(ctx, "_");
		PrintEscapedName(ctx, function->type->name);
	}
	else
	{
		PrintEscapedName(ctx, name);

		for(MatchData *alias = function->generics.head; alias; alias = alias->next)
		{
			Print(ctx, "_");
			PrintEscapedName(ctx, alias->type->name);
			Print(ctx, "_");
		}

		Print(ctx, "_%d", function->functionIndex);
	}

	if(!name.empty() && *(name.end - 1) == '$')
		Print(ctx, "_");
}

void TranslateVoid(ExpressionTranslateContext &ctx, ExprVoid *expression)
{
	(void)expression;

	Print(ctx, "/*void*/");
}

void TranslateBoolLiteral(ExpressionTranslateContext &ctx, ExprBoolLiteral *expression)
{
	if(expression->value)
		Print(ctx, "true");
	else
		Print(ctx, "false");
}

void TranslateCharacterLiteral(ExpressionTranslateContext &ctx, ExprCharacterLiteral *expression)
{
	Print(ctx, "char(%u)", expression->value);
}

void TranslateStringLiteral(ExpressionTranslateContext &ctx, ExprStringLiteral *expression)
{
	TranslateTypeName(ctx, expression->type);
	Print(ctx, "()");

	for(unsigned i = 0; i < expression->length; i++)
		Print(ctx, ".set(%d, %d)", i, expression->value[i]);
}

void TranslateIntegerLiteral(ExpressionTranslateContext &ctx, ExprIntegerLiteral *expression)
{
	if(expression->type == ctx.ctx.typeShort)
	{
		Print(ctx, "((short)%d)", short(expression->value));
	}
	else if(expression->type == ctx.ctx.typeInt)
	{
		Print(ctx, "%d", int(expression->value));
	}
	else if(expression->type == ctx.ctx.typeLong)
	{
		Print(ctx, "%lldll", expression->value);
	}
	else if(isType<TypeEnum>(expression->type))
	{
		TranslateTypeName(ctx, expression->type);
		Print(ctx, "(%d)", int(expression->value));
	}
	else
	{
		assert(!"unknown type");
	}
}

void TranslateRationalLiteral(ExpressionTranslateContext &ctx, ExprRationalLiteral *expression)
{
	if(expression->type == ctx.ctx.typeFloat)
		Print(ctx, "((float)%e)", expression->value);
	else if(expression->type == ctx.ctx.typeDouble)
		Print(ctx, "%e", expression->value);
	else
		assert(!"unknown type");
}

void TranslateTypeLiteral(ExpressionTranslateContext &ctx, ExprTypeLiteral *expression)
{
	Print(ctx, "__nullcTR[%d]", expression->value->typeIndex);
}

void TranslateNullptrLiteral(ExpressionTranslateContext &ctx, ExprNullptrLiteral *expression)
{
	Print(ctx, "(");
	TranslateTypeName(ctx, expression->type);
	Print(ctx, ")0");
}

void TranslateFunctionIndexLiteral(ExpressionTranslateContext &ctx, ExprFunctionIndexLiteral *expression)
{
	Print(ctx, "__nullcFR[%d]", expression->function->functionIndex);
}

void TranslatePassthrough(ExpressionTranslateContext &ctx, ExprPassthrough *expression)
{
	Translate(ctx, expression->value);
}

void TranslateArray(ExpressionTranslateContext &ctx, ExprArray *expression)
{
	TranslateTypeName(ctx, expression->type);
	Print(ctx, "()");

	unsigned index = 0;

	for(ExprBase *curr = expression->values.head; curr; curr = curr->next)
	{
		Print(ctx, ".set(%d, ", index);
		Translate(ctx, curr);
		Print(ctx, ")");

		index++;
	}
}

void TranslatePreModify(ExpressionTranslateContext &ctx, ExprPreModify *expression)
{
	Print(ctx, expression->isIncrement ? "++(*(" : "--(*(");
	Translate(ctx, expression->value);
	Print(ctx, "))");
}

void TranslatePostModify(ExpressionTranslateContext &ctx, ExprPostModify *expression)
{
	Print(ctx, "(*(");
	Translate(ctx, expression->value);
	Print(ctx, expression->isIncrement ? "))++" : "))--");
}

void TranslateCast(ExpressionTranslateContext &ctx, ExprTypeCast *expression)
{
	switch(expression->category)
	{
	case EXPR_CAST_NUMERICAL:
		Print(ctx, "(");
		TranslateTypeName(ctx, expression->type);
		Print(ctx, ")(");
		Translate(ctx, expression->value);
		Print(ctx, ")");
		break;
	case EXPR_CAST_PTR_TO_BOOL:
		Print(ctx, "(!!(");
		Translate(ctx, expression->value);
		Print(ctx, "))");
		break;
	case EXPR_CAST_UNSIZED_TO_BOOL:
		Print(ctx, "((");
		Translate(ctx, expression->value);
		Print(ctx, ").ptr != 0)");
		break;
	case EXPR_CAST_FUNCTION_TO_BOOL:
		Print(ctx, "((");
		Translate(ctx, expression->value);
		Print(ctx, ").id != 0)");
		break;
	case EXPR_CAST_NULL_TO_PTR:
		if(TypeRef *typeRef = getType<TypeRef>(expression->type))
		{
			Print(ctx, "(");
			TranslateTypeName(ctx, typeRef->subType);
			Print(ctx, "*)0");
		}
		break;
	case EXPR_CAST_NULL_TO_AUTO_PTR:
		Print(ctx, "__nullcMakeAutoRef(0, 0)");
		break;
	case EXPR_CAST_NULL_TO_UNSIZED:
		if(TypeUnsizedArray *typeUnsizedArray = getType<TypeUnsizedArray>(expression->type))
		{
			Print(ctx, "NULLCArray< ");
			TranslateTypeName(ctx, typeUnsizedArray->subType);
			Print(ctx, " >()");
		}
		break;
	case EXPR_CAST_NULL_TO_AUTO_ARRAY:
		Print(ctx, "__makeAutoArray(0, NULLCArray<void>())");
		break;
	case EXPR_CAST_NULL_TO_FUNCTION:
		TranslateTypeName(ctx, expression->type);
		Print(ctx, "()");
		break;
	case EXPR_CAST_ARRAY_PTR_TO_UNSIZED:
		if(TypeRef *typeRef = getType<TypeRef>(expression->value->type))
		{
			TypeArray *typeArray = getType<TypeArray>(typeRef->subType);

			assert(typeArray);
			assert(unsigned(typeArray->length) == typeArray->length);

			Print(ctx, "__makeNullcArray< ");
			TranslateTypeName(ctx, typeArray->subType);
			Print(ctx, " >(");
			Translate(ctx, expression->value);
			Print(ctx, ", %d)", (unsigned)typeArray->length);
		}
		break;
	case EXPR_CAST_PTR_TO_AUTO_PTR:
		if(TypeRef *typeRef = getType<TypeRef>(expression->value->type))
		{
			TypeClass *classType = getType<TypeClass>(typeRef->subType);

			if(classType && (classType->extendable || classType->baseClass))
			{
				Print(ctx, "__nullcMakeExtendableAutoRef(");
				Translate(ctx, expression->value);
				Print(ctx, ")");
			}
			else
			{
				Print(ctx, "__nullcMakeAutoRef(");
				Translate(ctx, expression->value);
				Print(ctx, ", __nullcTR[%d])", typeRef->subType->typeIndex);
			}
		}
		break;
	case EXPR_CAST_AUTO_PTR_TO_PTR:
		if(TypeRef *typeRef = getType<TypeRef>(expression->type))
		{
			Print(ctx, "(");
			TranslateTypeName(ctx, typeRef->subType);
			Print(ctx, "*)__nullcGetAutoRef(");
			Translate(ctx, expression->value);
			Print(ctx, ", __nullcTR[%d])", typeRef->subType->typeIndex);
		}
		break;
	case EXPR_CAST_UNSIZED_TO_AUTO_ARRAY:
		if(TypeUnsizedArray *typeUnsizedArray = getType<TypeUnsizedArray>(expression->value->type))
		{
			Print(ctx, "__makeAutoArray(__nullcTR[%d], ", typeUnsizedArray->subType->typeIndex);
			Translate(ctx, expression->value);
			Print(ctx, ")");
		}
		break;
	case EXPR_CAST_REINTERPRET:
		if(isType<TypeUnsizedArray>(expression->type) && isType<TypeUnsizedArray>(expression->value->type))
		{
			Translate(ctx, expression->value);
		}
		else if(expression->type == ctx.ctx.typeInt && expression->value->type == ctx.ctx.typeTypeID)
		{
			Print(ctx, "(int)(");
			Translate(ctx, expression->value);
			Print(ctx, ")");
		}
		else if(isType<TypeEnum>(expression->type) && expression->value->type == ctx.ctx.typeInt)
		{
			TranslateTypeName(ctx, expression->type);
			Print(ctx, "(");
			Translate(ctx, expression->value);
			Print(ctx, ")");
		}
		else if(expression->type == ctx.ctx.typeInt && isType<TypeEnum>(expression->value->type))
		{
			Translate(ctx, expression->value);
			Print(ctx, ".value");
		}
		else if(isType<TypeRef>(expression->type) && isType<TypeRef>(expression->value->type))
		{
			Print(ctx, "(");
			TranslateTypeName(ctx, expression->type);
			Print(ctx, ")(");
			Translate(ctx, expression->value);
			Print(ctx, ")");
		}
		else if(isType<TypeFunction>(expression->type) && isType<TypeFunction>(expression->value->type))
		{
			TranslateTypeName(ctx, expression->type);
			Print(ctx, "(");
			Translate(ctx, expression->value);
			Print(ctx, ")");
		}
		else
		{
			assert(!"unknown cast");
		}
		break;
	default:
		assert(!"unknown cast");
	}
}

void TranslateUnaryOp(ExpressionTranslateContext &ctx, ExprUnaryOp *expression)
{
	switch(expression->op)
	{
	case SYN_UNARY_OP_PLUS:
		Print(ctx, "+(");
		break;
	case SYN_UNARY_OP_NEGATE:
		Print(ctx, "-(");
		break;
	case SYN_UNARY_OP_BIT_NOT:
		Print(ctx, "~(");
		break;
	case SYN_UNARY_OP_LOGICAL_NOT:
		Print(ctx, "!(");
		break;
	default:
		assert(!"unknown type");
	}

	Translate(ctx, expression->value);
	Print(ctx, ")");
}

void TranslateBinaryOp(ExpressionTranslateContext &ctx, ExprBinaryOp *expression)
{
	if(expression->op == SYN_BINARY_OP_POW)
	{
		Print(ctx, "__nullcPow((");
		Translate(ctx, expression->lhs);
		Print(ctx, ")");

		if(isType<TypeEnum>(expression->lhs->type))
			Print(ctx, ".value");

		Print(ctx, ", (");
		Translate(ctx, expression->rhs);
		Print(ctx, ")");

		if(isType<TypeEnum>(expression->rhs->type))
			Print(ctx, ".value");

		Print(ctx, ")");
	}
	else if(expression->op == SYN_BINARY_OP_MOD && (expression->lhs->type == ctx.ctx.typeFloat || expression->lhs->type == ctx.ctx.typeDouble))
	{
		Print(ctx, "__nullcMod((");
		Translate(ctx, expression->lhs);
		Print(ctx, "), (");
		Translate(ctx, expression->rhs);
		Print(ctx, "))");
	}
	else if(expression->op == SYN_BINARY_OP_LOGICAL_XOR)
	{
		Print(ctx, "!!(");
		Translate(ctx, expression->lhs);
		Print(ctx, ")");

		if(isType<TypeEnum>(expression->lhs->type))
			Print(ctx, ".value");

		Print(ctx, " != !!(");
		Translate(ctx, expression->rhs);
		Print(ctx, ")");

		if(isType<TypeEnum>(expression->rhs->type))
			Print(ctx, ".value");
	}
	else
	{
		Print(ctx, "(");
		Translate(ctx, expression->lhs);
		Print(ctx, ")");

		if(isType<TypeEnum>(expression->lhs->type))
			Print(ctx, ".value");

		switch(expression->op)
		{
		case SYN_BINARY_OP_ADD:
			Print(ctx, " + ");
			break;
		case SYN_BINARY_OP_SUB:
			Print(ctx, " - ");
			break;
		case SYN_BINARY_OP_MUL:
			Print(ctx, " * ");
			break;
		case SYN_BINARY_OP_DIV:
			Print(ctx, " / ");
			break;
		case SYN_BINARY_OP_MOD:
			Print(ctx, " %% ");
			break;
		case SYN_BINARY_OP_SHL:
			Print(ctx, " << ");
			break;
		case SYN_BINARY_OP_SHR:
			Print(ctx, " >> ");
			break;
		case SYN_BINARY_OP_LESS:
			Print(ctx, " < ");
			break;
		case SYN_BINARY_OP_LESS_EQUAL:
			Print(ctx, " <= ");
			break;
		case SYN_BINARY_OP_GREATER:
			Print(ctx, " > ");
			break;
		case SYN_BINARY_OP_GREATER_EQUAL:
			Print(ctx, " >= ");
			break;
		case SYN_BINARY_OP_EQUAL:
			Print(ctx, " == ");
			break;
		case SYN_BINARY_OP_NOT_EQUAL:
			Print(ctx, " != ");
			break;
		case SYN_BINARY_OP_BIT_AND:
			Print(ctx, " & ");
			break;
		case SYN_BINARY_OP_BIT_OR:
			Print(ctx, " | ");
			break;
		case SYN_BINARY_OP_BIT_XOR:
			Print(ctx, " ^ ");
			break;
		case SYN_BINARY_OP_LOGICAL_AND:
			Print(ctx, " && ");
			break;
		case SYN_BINARY_OP_LOGICAL_OR:
			Print(ctx, " || ");
			break;
		default:
			assert(!"unknown type");
		}

		Print(ctx, "(");
		Translate(ctx, expression->rhs);
		Print(ctx, ")");

		if(isType<TypeEnum>(expression->rhs->type))
			Print(ctx, ".value");
	}
}

void TranslateGetAddress(ExpressionTranslateContext &ctx, ExprGetAddress *expression)
{
	Print(ctx, "&");
	TranslateVariableName(ctx, expression->variable);
}

void TranslateDereference(ExpressionTranslateContext &ctx, ExprDereference *expression)
{
	Print(ctx, "*(");
	Translate(ctx, expression->value);
	Print(ctx, ")");
}

void TranslateUnboxing(ExpressionTranslateContext &ctx, ExprUnboxing *expression)
{
	Translate(ctx, expression->value);
}

void TranslateConditional(ExpressionTranslateContext &ctx, ExprConditional *expression)
{
	Print(ctx, "(");
	Translate(ctx, expression->condition);
	Print(ctx, ") ? (");
	Translate(ctx, expression->trueBlock);
	Print(ctx, ") : (");
	Translate(ctx, expression->falseBlock);
	Print(ctx, ")");
}

void TranslateAssignment(ExpressionTranslateContext &ctx, ExprAssignment *expression)
{
	Print(ctx, "*(");
	Translate(ctx, expression->lhs);
	Print(ctx, ") = (");
	Translate(ctx, expression->rhs);
	Print(ctx, ")");
}

void TranslateMemberAccess(ExpressionTranslateContext &ctx, ExprMemberAccess *expression)
{
	Print(ctx, "&(");
	Translate(ctx, expression->value);
	Print(ctx, ")->");
	TranslateVariableName(ctx, expression->member);
}

void TranslateArrayIndex(ExpressionTranslateContext &ctx, ExprArrayIndex *expression)
{
	if(TypeUnsizedArray *typeUnsizedArray = getType<TypeUnsizedArray>(expression->value->type))
	{
		Print(ctx, "__nullcIndexUnsizedArray(");
		Translate(ctx, expression->value);
		Print(ctx, ", ");
		Translate(ctx, expression->index);
		Print(ctx, ", %d)", typeUnsizedArray->subType->size);
	}
	else
	{
		Print(ctx, "(");
		Translate(ctx, expression->value);
		Print(ctx, ")->index(");
		Translate(ctx, expression->index);
		Print(ctx, ")");
	}
}

void TranslateReturn(ExpressionTranslateContext &ctx, ExprReturn *expression)
{
	if(expression->coroutineStateUpdate)
	{
		Translate(ctx, expression->coroutineStateUpdate);
		Print(ctx, ";");
		PrintLine(ctx);
		PrintIndent(ctx);
	}

	ExprSequence *closures = getType<ExprSequence>(expression->closures);

	if(closures && !closures->expressions.head)
		closures = NULL;

	if(!ctx.currentFunction)
	{
		assert(!closures);

		if(expression->value->type == ctx.ctx.typeBool || expression->value->type == ctx.ctx.typeChar || expression->value->type == ctx.ctx.typeShort || expression->value->type == ctx.ctx.typeInt)
			Print(ctx, "__nullcOutputResultInt((int)(");
		else if(expression->value->type == ctx.ctx.typeLong)
			Print(ctx, "__nullcOutputResultLong((long long)(");
		else if(expression->value->type == ctx.ctx.typeFloat || expression->value->type == ctx.ctx.typeDouble)
			Print(ctx, "__nullcOutputResultDouble((double)(");
		else if(isType<TypeEnum>(expression->value->type))
			Print(ctx, "__nullcOutputResultInt((int)(");
		else
			assert(!"unknown global return type");

		Translate(ctx, expression->value);

		if(isType<TypeEnum>(expression->value->type))
			Print(ctx, ".value");

		Print(ctx, "));");
		PrintLine(ctx);
		PrintIndent(ctx);

		Print(ctx, "return 0;");
	}
	else
	{
		if(expression->value->type == ctx.ctx.typeVoid)
		{
			if(closures)
			{
				Translate(ctx, expression->closures);
				Print(ctx, ";");
				PrintLine(ctx);
				PrintIndent(ctx);
			}

			Translate(ctx, expression->value);
			Print(ctx, ";");
			PrintLine(ctx);
			PrintIndent(ctx);

			Print(ctx, "return;");
		}
		else if(closures)
		{
			Print(ctx, "__nullcReturnValue_%d = ", ctx.nextReturnValueId);
			Translate(ctx, expression->value);
			Print(ctx, ";");
			PrintLine(ctx);
			PrintIndent(ctx);

			Translate(ctx, expression->closures);
			Print(ctx, ";");
			PrintLine(ctx);
			PrintIndent(ctx);

			Print(ctx, "return __nullcReturnValue_%d;", ctx.nextReturnValueId);
		}
		else
		{
			Print(ctx, "return ");
			Translate(ctx, expression->value);
			Print(ctx, ";");
		}
	}
}

void TranslateYield(ExpressionTranslateContext &ctx, ExprYield *expression)
{
	if(expression->coroutineStateUpdate)
	{
		Translate(ctx, expression->coroutineStateUpdate);
		Print(ctx, ";");
		PrintLine(ctx);
		PrintIndent(ctx);
	}

	ExprSequence *closures = getType<ExprSequence>(expression->closures);

	if(closures && !closures->expressions.head)
		closures = NULL;

	if(expression->value->type == ctx.ctx.typeVoid)
	{
		if(closures)
		{
			Translate(ctx, expression->closures);
			Print(ctx, ";");
			PrintLine(ctx);
			PrintIndent(ctx);
		}

		Translate(ctx, expression->value);
		Print(ctx, ";");
		PrintLine(ctx);
		PrintIndent(ctx);

		Print(ctx, "return;");
	}
	else if(closures)
	{
		Print(ctx, "__nullcReturnValue_%d = ", ctx.nextReturnValueId);
		Translate(ctx, expression->value);
		Print(ctx, ";");
		PrintLine(ctx);
		PrintIndent(ctx);

		Translate(ctx, expression->closures);
		Print(ctx, ";");
		PrintLine(ctx);
		PrintIndent(ctx);

		Print(ctx, "return __nullcReturnValue_%d;", ctx.nextReturnValueId);
	}
	else
	{
		Print(ctx, "return ");
		Translate(ctx, expression->value);
		Print(ctx, ";");
	}

	PrintLine(ctx);

	Print(ctx, "yield_%d:", ctx.currentFunction->nextTranslateRestoreBlock++);
}

void TranslateVariableDefinition(ExpressionTranslateContext &ctx, ExprVariableDefinition *expression)
{
	Print(ctx, "/* Definition of variable '%.*s' */", FMT_ISTR(expression->variable->name));

	if(expression->initializer)
	{
		if(ExprBlock *blockInitializer = getType<ExprBlock>(expression->initializer))
		{
			// Translate block initializer as a sequence
			Print(ctx, "(");
			PrintLine(ctx);

			ctx.depth++;

			for(ExprBase *curr = blockInitializer->expressions.head; curr; curr = curr->next)
			{
				PrintIndent(ctx);

				Translate(ctx, curr);

				if(curr->next)
					Print(ctx, ",");

				PrintLine(ctx);
			}

			ctx.depth--;

			PrintIndent(ctx);
			Print(ctx, ")");
		}
		else
		{
			Translate(ctx, expression->initializer);
		}
	}
	else
	{
		Print(ctx, "0");
	}
}

void TranslateArraySetup(ExpressionTranslateContext &ctx, ExprArraySetup *expression)
{
	TypeRef *refType = getType<TypeRef>(expression->lhs->type);

	assert(refType);

	TypeArray *arrayType = getType<TypeArray>(refType->subType);

	assert(arrayType);

	Print(ctx, "__nullcSetupArray((");
	Translate(ctx, expression->lhs);
	Print(ctx, ")->ptr, %u, ", (unsigned)arrayType->length);
	Translate(ctx, expression->initializer);
	Print(ctx, ")");
}

void TranslateVariableDefinitions(ExpressionTranslateContext &ctx, ExprVariableDefinitions *expression)
{
	for(ExprVariableDefinition *value = expression->definitions.head; value; value = getType<ExprVariableDefinition>(value->next))
	{
		Translate(ctx, value);

		if(value->next)
			Print(ctx, ", ");
	}
}

void TranslateVariableAccess(ExpressionTranslateContext &ctx, ExprVariableAccess *expression)
{
	TranslateVariableName(ctx, expression->variable);
}

void TranslateFunctionContextAccess(ExpressionTranslateContext &ctx, ExprFunctionContextAccess *expression)
{
	TypeRef *refType = getType<TypeRef>(expression->function->contextType);

	assert(refType);

	TypeClass *classType = getType<TypeClass>(refType->subType);

	assert(classType);

	if(classType->size == 0)
	{
		Print(ctx, "(");
		TranslateTypeName(ctx, expression->type);
		Print(ctx, ")0");
	}
	else
	{
		TranslateVariableName(ctx, expression->function->contextVariable);
	}
}

void TranslateFunctionDefinition(ExpressionTranslateContext &ctx, ExprFunctionDefinition *expression)
{
	if(!ctx.skipFunctionDefinitions)
	{
		// Skip nested definitions
		ctx.skipFunctionDefinitions = true;

		FunctionData *function = expression->function;

		bool isStatic = false;
		bool isGeneric = false;

		if(function->scope != ctx.ctx.globalScope && !function->scope->ownerNamespace && !function->scope->ownerType)
			isStatic = true;
		else if(*function->name.begin == '$')
			isStatic = true;
		else if(function->isHidden)
			isStatic = true;
		else if(ctx.ctx.IsGenericInstance(function))
			isGeneric = true;

		if(isStatic)
			Print(ctx, "static ");
		else if(isGeneric)
			Print(ctx, "template<int I> ");

		TranslateTypeName(ctx, function->type->returnType);
		Print(ctx, " ");
		TranslateFunctionName(ctx, function);
		Print(ctx, "(");

		for(ExprVariableDefinition *curr = expression->arguments.head; curr; curr = getType<ExprVariableDefinition>(curr->next))
		{
			TranslateTypeName(ctx, curr->variable->type);
			Print(ctx, " ");
			TranslateVariableName(ctx, curr->variable);
			Print(ctx, ", ");
		}

		TranslateTypeName(ctx, expression->contextArgument->variable->type);
		Print(ctx, " ");
		TranslateVariableName(ctx, expression->contextArgument->variable);

		Print(ctx, ")");
		PrintLine(ctx);
		PrintIndentedLine(ctx, "{");
		ctx.depth++;

		for(unsigned k = 0; k < function->functionScope->allVariables.size(); k++)
		{
			VariableData *variable = function->functionScope->allVariables[k];

			// Don't need variables allocated by intermediate vm compilation
			if(variable->isVmAlloca)
				continue;

			if(variable->lookupOnly)
				continue;

			bool isArgument = false;

			for(ExprVariableDefinition *curr = expression->arguments.head; curr; curr = getType<ExprVariableDefinition>(curr->next))
			{
				if(variable == curr->variable)
				{
					isArgument = true;
					break;
				}
			}

			if(isArgument || variable == expression->contextArgument->variable)
				continue;

			PrintIndent(ctx);
			TranslateTypeName(ctx, variable->type);
			Print(ctx, " ");
			TranslateVariableName(ctx, variable);
			Print(ctx, ";");
			PrintLine(ctx);
		}

		if(function->type->returnType != ctx.ctx.typeVoid)
		{
			PrintIndent(ctx);
			TranslateTypeName(ctx, function->type->returnType);
			Print(ctx, " __nullcReturnValue_%d;", ctx.nextReturnValueId);
			PrintLine(ctx);
		}

		if(expression->coroutineStateRead)
		{
			PrintIndent(ctx);
			Print(ctx, "int __currJmpOffset = ");
			Translate(ctx, expression->coroutineStateRead);
			Print(ctx, ";");
			PrintLine(ctx);

			for(unsigned i = 0; i < function->yieldCount; i++)
			{
				PrintIndentedLine(ctx, "if(__currJmpOffset == %d)", i + 1);

				ctx.depth++;

				PrintIndentedLine(ctx, "goto yield_%d;", i + 1);

				ctx.depth--;
			}
		}

		for(ExprBase *value = expression->expressions.head; value; value = value->next)
		{
			PrintIndent(ctx);

			Translate(ctx, value);

			Print(ctx, ";");

			PrintLine(ctx);
		}


		ctx.depth--;

		PrintIndentedLine(ctx, "}");

		ctx.nextReturnValueId++;

		if(isGeneric)
		{
			Print(ctx, "template ");

			TranslateTypeName(ctx, function->type->returnType);
			Print(ctx, " ");
			TranslateFunctionName(ctx, function);
			Print(ctx, "<0>(");

			for(ExprVariableDefinition *curr = expression->arguments.head; curr; curr = getType<ExprVariableDefinition>(curr->next))
			{
				TranslateTypeName(ctx, curr->variable->type);
				Print(ctx, " ");
				TranslateVariableName(ctx, curr->variable);
				Print(ctx, ", ");
			}

			TranslateTypeName(ctx, expression->contextArgument->variable->type);
			Print(ctx, " ");
			TranslateVariableName(ctx, expression->contextArgument->variable);

			Print(ctx, ");");
			PrintLine(ctx);
		}

		ctx.skipFunctionDefinitions = false;
	}
	else
	{
		Print(ctx, "(");
		TranslateTypeName(ctx, expression->type);
		Print(ctx, ")");
		Print(ctx, "__nullcMakeFunction(__nullcFR[%d], 0)", expression->function->functionIndex);
	}
}

void TranslateGenericFunctionPrototype(ExpressionTranslateContext &ctx, ExprGenericFunctionPrototype *expression)
{
	Print(ctx, "/* Definition of generic function prototype '%.*s' */", FMT_ISTR(expression->function->name));

	if(!expression->contextVariables.head)
	{
		Print(ctx, "0");
		return;
	}

	Print(ctx, "(");
	PrintLine(ctx);

	ctx.depth++;

	for(ExprBase *curr = expression->contextVariables.head; curr; curr = curr->next)
	{
		PrintIndent(ctx);

		Translate(ctx, curr);

		if(curr->next)
			Print(ctx, ", ");

		PrintLine(ctx);
	}

	ctx.depth--;

	PrintIndent(ctx);
	Print(ctx, ")");
}

void TranslateFunctionAccess(ExpressionTranslateContext &ctx, ExprFunctionAccess *expression)
{
	Print(ctx, "(");
	TranslateTypeName(ctx, expression->type);
	Print(ctx, ")");
	Print(ctx, "__nullcMakeFunction(__nullcFR[%d], ", expression->function->functionIndex);
	Translate(ctx, expression->context);
	Print(ctx, ")");
}

void TranslateFunctionCall(ExpressionTranslateContext &ctx, ExprFunctionCall *expression)
{
	if(ExprFunctionAccess *functionAccess = getType<ExprFunctionAccess>(expression->function))
	{
		TranslateFunctionName(ctx, functionAccess->function);

		if(UseNonStaticTemplate(ctx, functionAccess->function))
			Print(ctx, "<0>");

		Print(ctx, "(");

		for(ExprBase *value = expression->arguments.head; value; value = value->next)
		{
			Translate(ctx, value);

			Print(ctx, ", ");
		}

		Translate(ctx, functionAccess->context);

		Print(ctx, ")");
	}
	else if(TypeFunction *typeFunction = getType<TypeFunction>(expression->function->type))
	{
		// TODO: side effects may be performed multiple times since the node is translated two times
		Print(ctx, "((");
		TranslateTypeName(ctx, typeFunction->returnType);
		Print(ctx, "(*)(");

		for(TypeHandle *curr = typeFunction->arguments.head; curr; curr = curr->next)
		{
			TranslateTypeName(ctx, curr->type);

			Print(ctx, ", ");
		}

		Print(ctx, "void*))(*__nullcFM)[(");
		Translate(ctx, expression->function);
		Print(ctx, ").id])(");

		for(ExprBase *value = expression->arguments.head; value; value = value->next)
		{
			Translate(ctx, value);

			Print(ctx, ", ");
		}

		Print(ctx, "(");
		Translate(ctx, expression->function);
		Print(ctx, ").context)");
	}
	else
	{
		assert(!"unknwon type");
	}
}

void TranslateAliasDefinition(ExpressionTranslateContext &ctx, ExprAliasDefinition *expression)
{
	Print(ctx, "/* Definition of class typedef '%.*s' */", FMT_ISTR(expression->alias->name));

	Print(ctx, "0");
}

void TranslateClassPrototype(ExpressionTranslateContext &ctx, ExprClassPrototype *expression)
{
	Print(ctx, "/* Definition of class prototype '%.*s' */", FMT_ISTR(expression->classType->name));

	Print(ctx, "0");
}

void TranslateGenericClassPrototype(ExpressionTranslateContext &ctx, ExprGenericClassPrototype *expression)
{
	Print(ctx, "/* Definition of generic class prototype '%.*s' */", FMT_ISTR(expression->genericProtoType->name));

	Print(ctx, "0");
}

void TranslateClassDefinition(ExpressionTranslateContext &ctx, ExprClassDefinition *expression)
{
	Print(ctx, "/* Definition of class '%.*s' */", FMT_ISTR(expression->classType->name));

	Print(ctx, "0");
}

void TranslateEnumDefinition(ExpressionTranslateContext &ctx, ExprEnumDefinition *expression)
{
	Print(ctx, "/* Definition of enum '%.*s' */", FMT_ISTR(expression->enumType->name));

	Print(ctx, "0");
}

void TranslateIfElse(ExpressionTranslateContext &ctx, ExprIfElse *expression)
{
	Print(ctx, "if(");
	Translate(ctx, expression->condition);
	Print(ctx, ")");
	PrintLine(ctx);

	PrintIndentedLine(ctx, "{");
	ctx.depth++;
	PrintIndent(ctx);

	Translate(ctx, expression->trueBlock);
	
	Print(ctx, ";");

	PrintLine(ctx);
	ctx.depth--;
	PrintIndentedLine(ctx, "}");

	if(expression->falseBlock)
	{
		PrintIndentedLine(ctx, "else");

		PrintIndentedLine(ctx, "{");
		ctx.depth++;
		PrintIndent(ctx);

		Translate(ctx, expression->falseBlock);

		Print(ctx, ";");

		PrintLine(ctx);
		ctx.depth--;
		PrintIndentedLine(ctx, "}");
	}
}

void TranslateFor(ExpressionTranslateContext &ctx, ExprFor *expression)
{
	unsigned loopId = ctx.nextLoopId++;
	ctx.loopIdStack.push_back(loopId);

	Translate(ctx, expression->initializer);
	Print(ctx, ";");
	PrintLine(ctx);

	PrintIndent(ctx);
	Print(ctx, "while(");
	Translate(ctx, expression->condition);
	Print(ctx, ")");
	PrintLine(ctx);

	PrintIndentedLine(ctx, "{");
	ctx.depth++;
	PrintIndent(ctx);

	Translate(ctx, expression->body);

	Print(ctx, ";");

	PrintLine(ctx);

	Print(ctx, "continue_%d:;", loopId);
	PrintLine(ctx);

	PrintIndentedLine(ctx, "// Increment");

	PrintIndent(ctx);
	Translate(ctx, expression->increment);

	Print(ctx, ";");

	PrintLine(ctx);
	ctx.depth--;
	PrintIndentedLine(ctx, "}");

	Print(ctx, "break_%d:", loopId);
	PrintLine(ctx);
	PrintIndent(ctx);

	ctx.loopIdStack.pop_back();
}

void TranslateWhile(ExpressionTranslateContext &ctx, ExprWhile *expression)
{
	unsigned loopId = ctx.nextLoopId++;
	ctx.loopIdStack.push_back(loopId);

	Print(ctx, "while(");
	Translate(ctx, expression->condition);
	Print(ctx, ")");
	PrintLine(ctx);

	PrintIndentedLine(ctx, "{");
	ctx.depth++;
	PrintIndent(ctx);

	Translate(ctx, expression->body);

	Print(ctx, ";");

	Print(ctx, "continue_%d:;", loopId);
	PrintLine(ctx);

	PrintLine(ctx);
	ctx.depth--;
	PrintIndentedLine(ctx, "}");

	Print(ctx, "break_%d:", loopId);
	PrintLine(ctx);
	PrintIndent(ctx);

	ctx.loopIdStack.pop_back();
}

void TranslateDoWhile(ExpressionTranslateContext &ctx, ExprDoWhile *expression)
{
	unsigned loopId = ctx.nextLoopId++;
	ctx.loopIdStack.push_back(loopId);

	Print(ctx, "do");
	PrintLine(ctx);

	PrintIndentedLine(ctx, "{");
	ctx.depth++;
	PrintIndent(ctx);

	Translate(ctx, expression->body);

	Print(ctx, ";");
	PrintLine(ctx);

	Print(ctx, "continue_%d:;", loopId);
	PrintLine(ctx);

	ctx.depth--;
	PrintIndentedLine(ctx, "}");

	PrintIndent(ctx);
	Print(ctx, "while(");
	Translate(ctx, expression->condition);
	Print(ctx, ");");
	PrintLine(ctx);

	Print(ctx, "break_%d:", loopId);
	PrintLine(ctx);
	PrintIndent(ctx);

	ctx.loopIdStack.pop_back();
}

void TranslateSwitch(ExpressionTranslateContext &ctx, ExprSwitch *expression)
{
	unsigned loopId = ctx.nextLoopId++;
	ctx.loopIdStack.push_back(loopId);

	Print(ctx, "{");
	PrintLine(ctx);
	ctx.depth++;

	PrintIndent(ctx);
	Translate(ctx, expression->condition);
	Print(ctx, ";");
	PrintLine(ctx);

	unsigned i;

	i = 0;
	for(ExprBase *curr = expression->cases.head; curr; curr = curr->next, i++)
	{
		PrintIndent(ctx);
		Print(ctx, "if(");
		Translate(ctx, curr);
		Print(ctx, ")");
		PrintLine(ctx);

		ctx.depth++;
		PrintIndentedLine(ctx, "goto switch_%d_case_%d;", loopId, i);
		ctx.depth--;
	}

	if(expression->defaultBlock)
		PrintIndentedLine(ctx, "goto switch_%d_default;", loopId);

	i = 0;
	for(ExprBase *curr = expression->blocks.head; curr; curr = curr->next, i++)
	{
		Print(ctx, "switch_%d_case_%d:", loopId, i);
		PrintLine(ctx);

		PrintIndent(ctx);
		Translate(ctx, curr);

		if(curr->next || expression->defaultBlock)
			PrintLine(ctx);
	}

	if(expression->defaultBlock)
	{
		Print(ctx, "switch_%d_default:", loopId);
		PrintLine(ctx);

		PrintIndent(ctx);
		Translate(ctx, expression->defaultBlock);
	}

	PrintLine(ctx);

	ctx.depth--;
	PrintIndentedLine(ctx, "}");

	Print(ctx, "break_%d:", loopId);
	PrintLine(ctx);
	PrintIndent(ctx);

	ctx.loopIdStack.pop_back();
}

void TranslateBreak(ExpressionTranslateContext &ctx, ExprBreak *expression)
{
	if(ExprSequence *closures = getType<ExprSequence>(expression->closures))
	{
		if(closures->expressions.head)
		{
			Translate(ctx, expression->closures);
			Print(ctx, ";");
			PrintLine(ctx);
			PrintIndent(ctx);
		}
	}

	Print(ctx, "goto break_%d;", ctx.loopIdStack[ctx.loopIdStack.size() - expression->depth]);
}

void TranslateContinue(ExpressionTranslateContext &ctx, ExprContinue *expression)
{
	if(ExprSequence *closures = getType<ExprSequence>(expression->closures))
	{
		if(closures->expressions.head)
		{
			Translate(ctx, expression->closures);
			Print(ctx, ";");
			PrintLine(ctx);
			PrintIndent(ctx);
		}
	}

	Print(ctx, "goto continue_%d;", ctx.loopIdStack[ctx.loopIdStack.size() - expression->depth]);
}

void TranslateBlock(ExpressionTranslateContext &ctx, ExprBlock *expression)
{
	Print(ctx, "{");
	PrintLine(ctx);

	ctx.depth++;

	for(ExprBase *value = expression->expressions.head; value; value = value->next)
	{
		PrintIndent(ctx);

		Translate(ctx, value);

		Print(ctx, ";");

		PrintLine(ctx);
	}

	if(expression->closures)
	{
		PrintIndentedLine(ctx, "// Closures");

		PrintIndent(ctx);

		Translate(ctx, expression->closures);
		Print(ctx, ";");

		PrintLine(ctx);
	}

	ctx.depth--;

	PrintIndent(ctx);
	Print(ctx, "}");
}

void TranslateSequence(ExpressionTranslateContext &ctx, ExprSequence *expression)
{
	if(!expression->expressions.head)
	{
		Print(ctx, "/*empty sequence*/");
		return;
	}

	Print(ctx, "(");
	PrintLine(ctx);

	ctx.depth++;

	for(ExprBase *curr = expression->expressions.head; curr; curr = curr->next)
	{
		PrintIndent(ctx);

		Translate(ctx, curr);

		if(curr->next)
			Print(ctx, ", ");

		PrintLine(ctx);
	}

	ctx.depth--;

	PrintIndent(ctx);
	Print(ctx, ")");
}

const char* GetModuleOutputPath(Allocator *allocator, InplaceStr moduleName)
{
	unsigned length = unsigned(strlen("import_") + moduleName.length() + strlen(".cpp") + 1);
	char *targetName = (char*)allocator->alloc(length);

	char *pos = targetName;

	strcpy(pos, "import_");
	pos += strlen(pos);

	memcpy(pos, moduleName.begin, moduleName.length());
	pos += moduleName.length();

	*pos = 0;

	for(unsigned i = 0; i < strlen(targetName); i++)
	{
		if(targetName[i] == '/' || targetName[i] == '.')
			targetName[i] = '_';
	}

	strcpy(pos, ".cpp");

	return targetName;
}

const char* GetModuleMainName(Allocator *allocator, InplaceStr moduleName)
{
	unsigned length = unsigned(moduleName.length() + + strlen("__init_") + 1);
	char *targetName = (char*)allocator->alloc(length);

	SafeSprintf(targetName, length, "__init_%.*s", FMT_ISTR(moduleName));

	for(unsigned i = 0; i < length; i++)
	{
		if(targetName[i] == '/' || targetName[i] == '.')
			targetName[i] = '_';
	}

	return targetName;
}

bool TranslateModuleImports(ExpressionTranslateContext &ctx, SmallArray<const char*, 32> &dependencies)
{
	// Translate all imports (expept base)
	for(unsigned i = 1; i < ctx.ctx.imports.size(); i++)
	{
		ModuleData *data = ctx.ctx.imports[i];

		PrintIndentedLine(ctx, "// Requires '%.*s'", FMT_ISTR(data->name));

		const char *targetName = GetModuleOutputPath(ctx.allocator, data->name);

		bool found = false;

		for(unsigned k = 0; k < dependencies.size(); k++)
		{
			if(strcmp(dependencies[k], targetName) == 0)
			{
				found = true;
				break;
			}
		}

		if(found)
			continue;

		dependencies.push_back(targetName);

		const char *importPath = BinaryCache::GetImportPath();

		InplaceStr path = GetImportPath(ctx.ctx.allocator, importPath, data->name);
		InplaceStr pathNoImport = importPath ? InplaceStr(path.begin + strlen(importPath)) : path;

		char filePath[1024];

		unsigned fileSize = 0;
		int needDelete = false;

		assert(path.length() < 1024);
		SafeSprintf(filePath, 1024, "%.*s", path.length(), path.begin);

		char *fileContent = (char*)NULLC::fileLoad(filePath, &fileSize, &needDelete);

		if(!fileContent)
		{
			assert(pathNoImport.length() < 1024);
			SafeSprintf(filePath, 1024, "%.*s", pathNoImport.length(), pathNoImport.begin);

			fileContent = (char*)NULLC::fileLoad(filePath, &fileSize, &needDelete);
		}

		if(!fileContent)
		{
			const char *bytecode = BinaryCache::GetBytecode(path.begin);

			if(!bytecode)
			{
				bytecode = BinaryCache::GetBytecode(pathNoImport.begin);
			}

			if(!bytecode)
			{
				SafeSprintf(ctx.errorBuf, ctx.errorBufSize, "ERROR: module '%.*s' input file '%s' could not be opened", FMT_ISTR(data->name));
				return false;
			}

			fileContent = FindSource((ByteCode*)bytecode);
		}

		CompilerContext compilerCtx(ctx.allocator);

		compilerCtx.errorBuf = ctx.errorBuf;
		compilerCtx.errorBufSize = ctx.errorBufSize;

		ExprModule* nestedModule = AnalyzeModuleFromSource(compilerCtx, fileContent);

		if(!nestedModule)
		{
			if(ctx.errorPos)
				ctx.errorPos = compilerCtx.errorPos;

			if(ctx.errorBuf && ctx.errorBufSize)
			{
				unsigned currLen = (unsigned)strlen(ctx.errorBuf);
				SafeSprintf(ctx.errorBuf + currLen, ctx.errorBufSize - currLen, " [in module '%.*s']", FMT_ISTR(data->name));
			}

			return false;
		}

		ExpressionTranslateContext nested(compilerCtx.exprCtx, compilerCtx.allocator);

		nested.file = fopen(targetName, "w");

		if(!nested.file)
		{
			SafeSprintf(ctx.errorBuf, ctx.errorBufSize, "ERROR: module '%.*s' output file '%s' could not be opened", filePath);

			return false;
		}

		nested.mainName = GetModuleMainName(ctx.allocator, data->name);

		nested.indent = ctx.indent;

		nested.errorBuf = ctx.errorBuf;
		nested.errorBufSize = ctx.errorBufSize;

		char outBufTmp[1024];
		nested.outBuf = outBufTmp;
		nested.outBufSize = 1024;

		if(!TranslateModule(nested, nestedModule, dependencies))
		{
			unsigned currLen = (unsigned)strlen(ctx.errorBuf);
			SafeSprintf(ctx.errorBuf + currLen, ctx.errorBufSize - currLen, " [in module '%.*s']", FMT_ISTR(data->name));

			fclose(nested.file);

			return false;
		}

		fclose(nested.file);
	}

	return true;
}

void TranslateModuleTypePrototypes(ExpressionTranslateContext &ctx)
{
	PrintIndentedLine(ctx, "// Type prototypes");

	for(unsigned i = 0; i < ctx.ctx.types.size(); i++)
	{
		TypeBase *type = ctx.ctx.types[i];

		if(type->isGeneric)
			continue;

		if(TypeStruct *typeStruct = getType<TypeStruct>(type))
		{
			Print(ctx, "struct ");
			PrintEscapedName(ctx, typeStruct->name);
			Print(ctx, ";");
			PrintLine(ctx);
		}
		else if(TypeArray *typeArray = getType<TypeArray>(type))
		{
			Print(ctx, "struct ");
			PrintEscapedName(ctx, typeArray->name);
			Print(ctx, ";");
			PrintLine(ctx);
		}
	}

	PrintLine(ctx);
}

void TranslateModuleTypeDefinitions(ExpressionTranslateContext &ctx)
{
	PrintIndentedLine(ctx, "// Type definitions");
	PrintIndentedLine(ctx, "#pragma pack(push, 4)");

	for(unsigned i = 0; i < ctx.ctx.types.size(); i++)
	{
		TypeBase *type = ctx.ctx.types[i];

		TranslateTypeDefinition(ctx, type);
	}
	PrintIndentedLine(ctx, "#pragma pack(pop)");

	PrintLine(ctx);
}

void TranslateModuleFunctionPrototypes(ExpressionTranslateContext &ctx)
{
	PrintIndentedLine(ctx, "// Function prototypes");

	for(unsigned i = 0; i < ctx.ctx.functions.size(); i++)
	{
		FunctionData *function = ctx.ctx.functions[i];

		if(ctx.ctx.IsGenericFunction(function))
			continue;

		if(function->implementation)
			continue;

		bool isStatic = false;
		bool isGeneric = false;

		if(function->scope != ctx.ctx.globalScope && !function->scope->ownerNamespace && !function->scope->ownerType)
			isStatic = true;
		else if(*function->name.begin == '$')
			isStatic = true;
		else if(function->isHidden)
			isStatic = true;
		else if(ctx.ctx.IsGenericInstance(function))
			isGeneric = true;

		if(isStatic && function->importModule)
			continue;

		if(isStatic)
			Print(ctx, "static ");
		else if(isGeneric)
			Print(ctx, "template<int I> ");

		TranslateTypeName(ctx, function->type->returnType);
		Print(ctx, " ");
		TranslateFunctionName(ctx, function);
		Print(ctx, "(");

		if(function->importModule)
		{
			for(unsigned i = 0; i < function->arguments.size(); i++)
			{
				ArgumentData &argument = function->arguments[i];

				TranslateTypeName(ctx, argument.type);
				Print(ctx, " ");

				if(argument.name == InplaceStr("this"))
				{
					Print(ctx, "__context");
				}
				else if(*argument.name.begin == '$')
				{
					InplaceStr name = InplaceStr(argument.name.begin + 1, argument.name.end);

					Print(ctx, "__%.*s", FMT_ISTR(name));
				}
				else
				{
					Print(ctx, "%.*s", FMT_ISTR(argument.name));
				}

				Print(ctx, ", ");
			}

			TranslateTypeName(ctx, function->contextType);
			Print(ctx, " __context");
		}
		else
		{
			for(VariableHandle *curr = function->argumentVariables.head; curr; curr = curr->next)
			{
				TranslateTypeName(ctx, curr->variable->type);
				Print(ctx, " ");
				TranslateVariableName(ctx, curr->variable);
				Print(ctx, ", ");
			}

			TranslateTypeName(ctx, function->contextArgument->type);
			Print(ctx, " ");
			TranslateVariableName(ctx, function->contextArgument);
		}

		Print(ctx, ");");
		PrintLine(ctx);
	}

	PrintLine(ctx);
}

void TranslateModuleGlobalVariables(ExpressionTranslateContext &ctx)
{
	PrintIndentedLine(ctx, "// Global variables");

	for(unsigned int i = 0; i < ctx.ctx.variables.size(); i++)
	{
		VariableData *variable = ctx.ctx.variables[i];

		// Don't need variables allocated by intermediate vm compilation
		if(variable->isVmAlloca)
			continue;

		if(variable->importModule)
		{
			Print(ctx, "extern ");
			TranslateTypeName(ctx, variable->type);
			Print(ctx, " ");
			TranslateVariableName(ctx, variable);
			Print(ctx, ";");
			PrintLine(ctx);
		}
		else if(variable->scope == ctx.ctx.globalScope || variable->scope->ownerNamespace)
		{
			TranslateTypeName(ctx, variable->type);
			Print(ctx, " ");
			TranslateVariableName(ctx, variable);
			Print(ctx, ";");
			PrintLine(ctx);
		}
		else if(ctx.ctx.GlobalScopeFrom(variable->scope))
		{
			Print(ctx, "static ");
			TranslateTypeName(ctx, variable->type);
			Print(ctx, " ");
			TranslateVariableName(ctx, variable);
			Print(ctx, ";");
			PrintLine(ctx);
		}
	}

	PrintLine(ctx);
}

void TranslateModuleFunctionDefinitions(ExpressionTranslateContext &ctx, ExprModule *expression)
{
	PrintIndentedLine(ctx, "// Function definitions");

	for(unsigned i = 0; i < expression->definitions.size(); i++)
	{
		ExprFunctionDefinition *definition = getType<ExprFunctionDefinition>(expression->definitions[i]);

		if(definition->function->isPrototype)
			continue;

		ctx.currentFunction = definition->function;

		Translate(ctx, definition);

		PrintLine(ctx);
	}

	ctx.currentFunction = NULL;

	ctx.skipFunctionDefinitions = true;

	PrintLine(ctx);
}

void TranslateModuleTypeInformation(ExpressionTranslateContext &ctx)
{
	PrintIndentedLine(ctx, "// Register types");

	for(unsigned i = 0; i < ctx.ctx.types.size(); i++)
	{
		TypeBase *type = ctx.ctx.types[i];

		if(type->isGeneric)
		{
			PrintIndentedLine(ctx, "__nullcTR[%d] = 0; // generic type '%.*s'", i, FMT_ISTR(type->name));
			continue;
		}

		PrintIndent(ctx);
		Print(ctx, "__nullcTR[%d] = __nullcRegisterType(", i);
		Print(ctx, "%uu, ", type->nameHash);
		Print(ctx, "\"%.*s\", ", FMT_ISTR(type->name));
		Print(ctx, "%d, ", type->size);

		if(TypeArray *typeArray = getType<TypeArray>(type))
		{
			Print(ctx, "__nullcTR[%d], ", typeArray->subType->typeIndex);
			Print(ctx, "%d, NULLC_ARRAY, %d, 0);", (unsigned)typeArray->length, typeArray->alignment);
		}
		else if(TypeUnsizedArray *typeUnsizedArray = getType<TypeUnsizedArray>(type))
		{
			Print(ctx, "__nullcTR[%d], ", typeUnsizedArray->subType->typeIndex);
			Print(ctx, "-1, NULLC_ARRAY, %d, 0);", typeUnsizedArray->alignment);
		}
		else if(TypeRef *typeRef = getType<TypeRef>(type))
		{
			Print(ctx, "__nullcTR[%d], ", typeRef->subType->typeIndex);
			Print(ctx, "1, NULLC_POINTER, %d, 0);", typeRef->alignment);
		}
		else if(TypeFunction *typeFunction = getType<TypeFunction>(type))
		{
			Print(ctx, "__nullcTR[%d], ", typeFunction->returnType->typeIndex);
			Print(ctx, "0, NULLC_FUNCTION, %d, 0);", typeFunction->alignment);
		}
		else if(TypeClass *typeClass = getType<TypeClass>(type))
		{
			unsigned count = 0;

			for(VariableHandle *curr = typeClass->members.head; curr; curr = curr->next)
			{
				if(*curr->variable->name.begin == '$')
					continue;

				count++;
			}

			Print(ctx, "__nullcTR[%d], ", typeClass->baseClass ? typeClass->baseClass->typeIndex : 0);
			Print(ctx, "%d, NULLC_CLASS, %d, ", count, typeClass->alignment);

			if(typeClass->hasFinalizer && typeClass->extendable)
				Print(ctx, "NULLC_TYPE_FLAG_HAS_FINALIZER | NULLC_TYPE_FLAG_IS_EXTENDABLE");
			else if(typeClass->hasFinalizer)
				Print(ctx, "NULLC_TYPE_FLAG_HAS_FINALIZER");
			else if(typeClass->extendable)
				Print(ctx, "NULLC_TYPE_FLAG_IS_EXTENDABLE");
			else
				Print(ctx, "0");

			Print(ctx, ");", count, typeClass->alignment);
		}
		else if(TypeStruct *typeStruct = getType<TypeStruct>(type))
		{
			unsigned count = 0;

			for(VariableHandle *curr = typeStruct->members.head; curr; curr = curr->next)
			{
				if(*curr->variable->name.begin == '$')
					continue;

				count++;
			}

			Print(ctx, "__nullcTR[0], ");
			Print(ctx, "%d, NULLC_CLASS, %d, 0);", count, typeStruct->alignment);
		}
		else
		{
			Print(ctx, "__nullcTR[0], ");
			Print(ctx, "0, NULLC_NONE, %d, 0);", type->alignment);
		}

		PrintLine(ctx);
	}

	PrintLine(ctx);

	PrintIndentedLine(ctx, "// Register type members");

	for(unsigned i = 0; i < ctx.ctx.types.size(); i++)
	{
		TypeBase *type = ctx.ctx.types[i];

		if(type->isGeneric)
			continue;

		if(TypeFunction *typeFunction = getType<TypeFunction>(type))
		{
			PrintIndent(ctx);
			Print(ctx, "__nullcRegisterMembers(__nullcTR[%d], %d", i, typeFunction->arguments.size());

			for(TypeHandle *curr = typeFunction->arguments.head; curr; curr = curr->next)
			{
				Print(ctx, ", __nullcTR[%d], 0", curr->type->typeIndex);
				Print(ctx, ", 0");
				Print(ctx, ", \"\"");
			}

			Print(ctx, "); // type '%.*s' arguments", FMT_ISTR(type->name));
			PrintLine(ctx);
		}
		else if(TypeStruct *typeStruct = getType<TypeStruct>(type))
		{
			unsigned count = 0;

			for(VariableHandle *curr = typeStruct->members.head; curr; curr = curr->next)
			{
				if(*curr->variable->name.begin == '$')
					continue;

				count++;
			}

			PrintIndent(ctx);
			Print(ctx, "__nullcRegisterMembers(__nullcTR[%d], %d", i, count);

			for(VariableHandle *curr = typeStruct->members.head; curr; curr = curr->next)
			{
				if(*curr->variable->name.begin == '$')
					continue;

				Print(ctx, ", __nullcTR[%d]", curr->variable->type->typeIndex);
				Print(ctx, ", %d", curr->variable->offset);
				Print(ctx, ", \"%.*s\"", FMT_ISTR(curr->variable->name));
			}

			Print(ctx, "); // type '%.*s' members", FMT_ISTR(type->name));
			PrintLine(ctx);
		}
	}

	PrintLine(ctx);
}

void TranslateModuleGlobalVariableInformation(ExpressionTranslateContext &ctx)
{
	PrintIndentedLine(ctx, "// Register globals");

	for(unsigned int i = 0; i < ctx.ctx.variables.size(); i++)
	{
		VariableData *variable = ctx.ctx.variables[i];

		// Don't need variables allocated by intermediate vm compilation
		if(variable->isVmAlloca)
			continue;

		if(variable->importModule)
			continue;

		if(variable->scope == ctx.ctx.globalScope || variable->scope->ownerNamespace || ctx.ctx.GlobalScopeFrom(variable->scope))
		{
			PrintIndent(ctx);
			Print(ctx, "__nullcRegisterGlobal((void*)&");
			TranslateVariableName(ctx, variable);
			Print(ctx, ", __nullcTR[%d]);", variable->type->typeIndex);
			PrintLine(ctx);
		}
	}

	PrintLine(ctx);
}

void TranslateModuleGlobalFunctionInformation(ExpressionTranslateContext &ctx)
{
	PrintIndentedLine(ctx, "// Register functions");

	for(unsigned i = 0; i < ctx.ctx.functions.size(); i++)
	{
		FunctionData *function = ctx.ctx.functions[i];

		if(ctx.ctx.IsGenericFunction(function))
		{
			PrintIndentedLine(ctx, "__nullcFR[%d] = 0; // generic function '%.*s'", i, FMT_ISTR(function->name));
			continue;
		}

		bool isStatic = false;

		if(function->scope != ctx.ctx.globalScope && !function->scope->ownerNamespace && !function->scope->ownerType)
			isStatic = true;
		else if(*function->name.begin == '$')
			isStatic = true;
		else if(function->isHidden)
			isStatic = true;

		if(isStatic && function->importModule)
		{
			PrintIndentedLine(ctx, "__nullcFR[%d] = 0; // module '%.*s' internal function '%.*s'", i, FMT_ISTR(function->importModule->name), FMT_ISTR(function->name));
			continue;
		}

		PrintIndent(ctx);
		Print(ctx, "__nullcFR[%d] = __nullcRegisterFunction(\"", i);
		TranslateFunctionName(ctx, function);
		Print(ctx, "\", (void*)");
		TranslateFunctionName(ctx, function);

		if(UseNonStaticTemplate(ctx, function))
			Print(ctx, "<0>");

		if(function->contextType)
			Print(ctx, ", __nullcTR[%d]", function->contextType->typeIndex);
		else
			Print(ctx, ", -1");

		if(function->scope->ownerType)
			Print(ctx, ", FunctionCategory::THISCALL);");
		else if(function->coroutine)
			Print(ctx, ", FunctionCategory::COROUTINE);");
		else if(function->contextType != ctx.ctx.typeVoid->refType)
			Print(ctx, ", FunctionCategory::LOCAL);");
		else
			Print(ctx, ", FunctionCategory::NORMAL);");
		PrintLine(ctx);
	}

	PrintLine(ctx);
}

bool TranslateModule(ExpressionTranslateContext &ctx, ExprModule *expression, SmallArray<const char*, 32> &dependencies)
{
	if(!TranslateModuleImports(ctx, dependencies))
		return false;

	// Generate type indexes
	for(unsigned i = 0; i < ctx.ctx.types.size(); i++)
	{
		TypeBase *type = ctx.ctx.types[i];

		type->typeIndex = i;

		type->hasTranslation = false;
	}

	// Generate function indexes
	for(unsigned i = 0; i < ctx.ctx.functions.size(); i++)
	{
		FunctionData *function = ctx.ctx.functions[i];

		function->functionIndex = i;

		function->nextTranslateRestoreBlock = 1;
	}

	PrintIndentedLine(ctx, "#include \"runtime.h\"");
	PrintIndentedLine(ctx, "// Typeid redirect table");
	PrintIndentedLine(ctx, "static unsigned __nullcTR[%d];", ctx.ctx.types.size());
	PrintIndentedLine(ctx, "// Function pointer table");
	PrintIndentedLine(ctx, "static __nullcFunctionArray* __nullcFM;");
	PrintIndentedLine(ctx, "// Function pointer redirect table");
	PrintIndentedLine(ctx, "static unsigned __nullcFR[%d];", ctx.ctx.functions.size());
	PrintLine(ctx);

	TranslateModuleTypePrototypes(ctx);

	TranslateModuleTypeDefinitions(ctx);
	
	TranslateModuleFunctionPrototypes(ctx);

	TranslateModuleGlobalVariables(ctx);
	
	TranslateModuleFunctionDefinitions(ctx, expression);

	PrintIndentedLine(ctx, "// Module initializers");

	for(unsigned i = 1; i < ctx.ctx.imports.size(); i++)
	{
		ModuleData *data = ctx.ctx.imports[i];

		PrintIndentedLine(ctx, "extern int %s();", GetModuleMainName(ctx.allocator, data->name));
	}

	PrintLine(ctx);
	PrintIndentedLine(ctx, "// Global code");

	PrintIndentedLine(ctx, "int %s()", ctx.mainName);
	PrintIndentedLine(ctx, "{");

	ctx.depth++;

	PrintIndentedLine(ctx, "static int moduleInitialized = 0;");
	PrintIndentedLine(ctx, "if(moduleInitialized++)");

	ctx.depth++;
	PrintIndentedLine(ctx, "return 0;");
	ctx.depth--;

	PrintIndentedLine(ctx, "__nullcFM = __nullcGetFunctionTable();");
	PrintIndentedLine(ctx, "int __local = 0;");
	PrintIndentedLine(ctx, "__nullcRegisterBase(&__local);");
	PrintIndentedLine(ctx, "__nullcInitBaseModule();");

	PrintLine(ctx);
	PrintIndentedLine(ctx, "// Initialize modules");

	for(unsigned i = 1; i < ctx.ctx.imports.size(); i++)
	{
		ModuleData *data = ctx.ctx.imports[i];

		PrintIndentedLine(ctx, "%s();", GetModuleMainName(ctx.allocator, data->name));
	}

	PrintLine(ctx);

	TranslateModuleTypeInformation(ctx);
	
	TranslateModuleGlobalVariableInformation(ctx);
	
	TranslateModuleGlobalFunctionInformation(ctx);
	
	PrintIndentedLine(ctx, "// Setup");

	for(ExprBase *value = expression->setup.head; value; value = value->next)
	{
		PrintIndent(ctx);

		Translate(ctx, value);

		Print(ctx, ";");

		PrintLine(ctx);
	}

	PrintLine(ctx);
	PrintIndentedLine(ctx, "// Expressions");

	for(ExprBase *value = expression->expressions.head; value; value = value->next)
	{
		PrintIndent(ctx);

		Translate(ctx, value);

		Print(ctx, ";");

		PrintLine(ctx);
	}

	PrintIndentedLine(ctx, "return 0;");

	ctx.depth--;

	PrintIndentedLine(ctx, "}");

	FlushBuffered(ctx);

	return true;
}

void Translate(ExpressionTranslateContext &ctx, ExprBase *expression)
{
	if(ExprVoid *expr = getType<ExprVoid>(expression))
		TranslateVoid(ctx, expr);
	else if(ExprBoolLiteral *expr = getType<ExprBoolLiteral>(expression))
		TranslateBoolLiteral(ctx, expr);
	else if(ExprCharacterLiteral *expr = getType<ExprCharacterLiteral>(expression))
		TranslateCharacterLiteral(ctx, expr);
	else if(ExprStringLiteral *expr = getType<ExprStringLiteral>(expression))
		TranslateStringLiteral(ctx, expr);
	else if(ExprIntegerLiteral *expr = getType<ExprIntegerLiteral>(expression))
		TranslateIntegerLiteral(ctx, expr);
	else if(ExprRationalLiteral *expr = getType<ExprRationalLiteral>(expression))
		TranslateRationalLiteral(ctx, expr);
	else if(ExprTypeLiteral *expr = getType<ExprTypeLiteral>(expression))
		TranslateTypeLiteral(ctx, expr);
	else if(ExprNullptrLiteral *expr = getType<ExprNullptrLiteral>(expression))
		TranslateNullptrLiteral(ctx, expr);
	else if(ExprFunctionIndexLiteral *expr = getType<ExprFunctionIndexLiteral>(expression))
		TranslateFunctionIndexLiteral(ctx, expr);
	else if(ExprPassthrough *expr = getType<ExprPassthrough>(expression))
		TranslatePassthrough(ctx, expr);
	else if(ExprArray *expr = getType<ExprArray>(expression))
		TranslateArray(ctx, expr);
	else if(ExprPreModify *expr = getType<ExprPreModify>(expression))
		TranslatePreModify(ctx, expr);
	else if(ExprPostModify *expr = getType<ExprPostModify>(expression))
		TranslatePostModify(ctx, expr);
	else if(ExprTypeCast *expr = getType<ExprTypeCast>(expression))
		TranslateCast(ctx, expr);
	else if(ExprUnaryOp *expr = getType<ExprUnaryOp>(expression))
		TranslateUnaryOp(ctx, expr);
	else if(ExprBinaryOp *expr = getType<ExprBinaryOp>(expression))
		TranslateBinaryOp(ctx, expr);
	else if(ExprGetAddress *expr = getType<ExprGetAddress>(expression))
		TranslateGetAddress(ctx, expr);
	else if(ExprDereference *expr = getType<ExprDereference>(expression))
		TranslateDereference(ctx, expr);
	else if(ExprUnboxing *expr = getType<ExprUnboxing>(expression))
		TranslateUnboxing(ctx, expr);
	else if(ExprConditional *expr = getType<ExprConditional>(expression))
		TranslateConditional(ctx, expr);
	else if(ExprAssignment *expr = getType<ExprAssignment>(expression))
		TranslateAssignment(ctx, expr);
	else if(ExprMemberAccess *expr = getType<ExprMemberAccess>(expression))
		TranslateMemberAccess(ctx, expr);
	else if(ExprArrayIndex *expr = getType<ExprArrayIndex>(expression))
		TranslateArrayIndex(ctx, expr);
	else if(ExprReturn *expr = getType<ExprReturn>(expression))
		TranslateReturn(ctx, expr);
	else if(ExprYield *expr = getType<ExprYield>(expression))
		TranslateYield(ctx, expr);
	else if(ExprVariableDefinition *expr = getType<ExprVariableDefinition>(expression))
		TranslateVariableDefinition(ctx, expr);
	else if(ExprArraySetup *expr = getType<ExprArraySetup>(expression))
		TranslateArraySetup(ctx, expr);
	else if(ExprVariableDefinitions *expr = getType<ExprVariableDefinitions>(expression))
		TranslateVariableDefinitions(ctx, expr);
	else if(ExprVariableAccess *expr = getType<ExprVariableAccess>(expression))
		TranslateVariableAccess(ctx, expr);
	else if(ExprFunctionContextAccess *expr = getType<ExprFunctionContextAccess>(expression))
		TranslateFunctionContextAccess(ctx, expr);
	else if(ExprFunctionDefinition *expr = getType<ExprFunctionDefinition>(expression))
		TranslateFunctionDefinition(ctx, expr);
	else if(ExprGenericFunctionPrototype *expr = getType<ExprGenericFunctionPrototype>(expression))
		TranslateGenericFunctionPrototype(ctx, expr);
	else if(ExprFunctionAccess *expr = getType<ExprFunctionAccess>(expression))
		TranslateFunctionAccess(ctx, expr);
	else if(ExprFunctionOverloadSet *expr = getType<ExprFunctionOverloadSet>(expression))
		assert(!"miscompiled tree");
	else if(ExprFunctionCall *expr = getType<ExprFunctionCall>(expression))
		TranslateFunctionCall(ctx, expr);
	else if(ExprAliasDefinition *expr = getType<ExprAliasDefinition>(expression))
		TranslateAliasDefinition(ctx, expr);
	else if(ExprClassPrototype *expr = getType<ExprClassPrototype>(expression))
		TranslateClassPrototype(ctx, expr);
	else if(ExprGenericClassPrototype *expr = getType<ExprGenericClassPrototype>(expression))
		TranslateGenericClassPrototype(ctx, expr);
	else if(ExprClassDefinition *expr = getType<ExprClassDefinition>(expression))
		TranslateClassDefinition(ctx, expr);
	else if(ExprEnumDefinition *expr = getType<ExprEnumDefinition>(expression))
		TranslateEnumDefinition(ctx, expr);
	else if(ExprIfElse *expr = getType<ExprIfElse>(expression))
		TranslateIfElse(ctx, expr);
	else if(ExprFor *expr = getType<ExprFor>(expression))
		TranslateFor(ctx, expr);
	else if(ExprWhile *expr = getType<ExprWhile>(expression))
		TranslateWhile(ctx, expr);
	else if(ExprDoWhile *expr = getType<ExprDoWhile>(expression))
		TranslateDoWhile(ctx, expr);
	else if(ExprSwitch *expr = getType<ExprSwitch>(expression))
		TranslateSwitch(ctx, expr);
	else if(ExprBreak *expr = getType<ExprBreak>(expression))
		TranslateBreak(ctx, expr);
	else if(ExprContinue *expr = getType<ExprContinue>(expression))
		TranslateContinue(ctx, expr);
	else if(ExprBlock *expr = getType<ExprBlock>(expression))
		TranslateBlock(ctx, expr);
	else if(ExprSequence *expr = getType<ExprSequence>(expression))
		TranslateSequence(ctx, expr);
	else
		assert(!"unknown type");
}
