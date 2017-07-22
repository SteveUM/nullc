#pragma once

#include "stdafx.h"
#include "IntrusiveList.h"

struct TypeBase;
struct ExprBase;
struct VariableData;
struct FunctionData;
struct ScopeData;

struct ExpressionContext;

struct VmValue;
struct VmConstant;
struct VmInstruction;
struct VmBlock;
struct VmFunction;
struct VmModule;

enum VmValueType
{
	VM_TYPE_VOID,

	VM_TYPE_INT,
	VM_TYPE_DOUBLE,
	VM_TYPE_LONG,

	VM_TYPE_LABEL,

	VM_TYPE_POINTER,
	VM_TYPE_FUNCTION_REF,
	VM_TYPE_ARRAY_REF,
	VM_TYPE_AUTO_REF,
	VM_TYPE_AUTO_ARRAY,

	VM_TYPE_STRUCT,
};

enum VmInstructionType
{
	VM_INST_LOAD_BYTE,
	VM_INST_LOAD_SHORT,
	VM_INST_LOAD_INT,
	VM_INST_LOAD_FLOAT,
	VM_INST_LOAD_DOUBLE,
	VM_INST_LOAD_LONG,
	VM_INST_LOAD_STRUCT,

	VM_INST_LOAD_IMMEDIATE,

	VM_INST_STORE_BYTE,
	VM_INST_STORE_SHORT,
	VM_INST_STORE_INT,
	VM_INST_STORE_FLOAT,
	VM_INST_STORE_DOUBLE,
	VM_INST_STORE_LONG,
	VM_INST_STORE_STRUCT,

	VM_INST_DOUBLE_TO_INT,
	VM_INST_DOUBLE_TO_LONG,
	VM_INST_DOUBLE_TO_FLOAT,
	VM_INST_INT_TO_DOUBLE,
	VM_INST_LONG_TO_DOUBLE,
	VM_INST_INT_TO_LONG,
	VM_INST_LONG_TO_INT,

	VM_INST_INDEX, // pointer, array_size, element_size, index
	VM_INST_INDEX_UNSIZED,

	VM_INST_FUNCTION_ADDRESS,
	VM_INST_TYPE_ID,

	VM_INST_SET_RANGE,

	VM_INST_JUMP,
	VM_INST_JUMP_Z,
	VM_INST_JUMP_NZ,

	VM_INST_CALL,

	VM_INST_RETURN,
	VM_INST_YIELD,

	VM_INST_ADD,
	VM_INST_SUB,
	VM_INST_MUL,
	VM_INST_DIV,
	VM_INST_POW,
	VM_INST_MOD,
	VM_INST_LESS,
	VM_INST_GREATER,
	VM_INST_LESS_EQUAL,
	VM_INST_GREATER_EQUAL,
	VM_INST_EQUAL,
	VM_INST_NOT_EQUAL,
	VM_INST_SHL,
	VM_INST_SHR,
	VM_INST_BIT_AND,
	VM_INST_BIT_OR,
	VM_INST_BIT_XOR,
	VM_INST_LOG_XOR,

	VM_INST_NEG,
	VM_INST_BIT_NOT,
	VM_INST_LOG_NOT,

	VM_INST_CREATE_CLOSURE,
	VM_INST_CLOSE_UPVALUES,
	VM_INST_CONVERT_POINTER,
	VM_INST_CHECKED_RETURN,

	VM_INST_CONSTRUCT, // Pseudo instruction to collect multiple elements into a single value
	VM_INST_EXTRACT, // Pseudo instruction to extract an element from a composite value

	VM_INST_PHI, // Pseudo instruction to create a value based on control flow
};

enum VmPassType
{
	VM_PASS_OPT_PEEPHOLE,
	VM_PASS_OPT_CONSTANT_PROPAGATION,
	VM_PASS_OPT_DEAD_CODE_ELIMINATION,
	VM_PASS_OPT_CONTROL_FLOW_SIPLIFICATION,
	VM_PASS_OPT_LOAD_STORE_PROPAGATION,
	VM_PASS_OPT_COMMON_SUBEXPRESSION_ELIMINATION,

	VM_PASS_CREATE_ALLOCA_STORAGE,

	VM_PASS_LEGALIZE_VM,
};

struct VmType
{
	VmType(VmValueType type, unsigned size, TypeBase *structType): type(type), size(size), structType(structType)
	{
	}

	bool operator==(const VmType& rhs) const
	{
		return type == rhs.type && size == rhs.size;
	}
	bool operator!=(const VmType& rhs) const
	{
		return !(*this == rhs);
	}

	VmValueType type;
	unsigned size;

	TypeBase *structType;

	static const VmType Void;
	static const VmType Int;
	static const VmType Double;
	static const VmType Long;
	static const VmType Label;
	static const VmType AutoRef;
	static const VmType AutoArray;

	static const VmType Pointer(TypeBase *structType)
	{
		assert(structType);

		return VmType(VM_TYPE_POINTER, NULLC_PTR_SIZE, structType);
	}

	static const VmType FunctionRef(TypeBase *structType)
	{
		assert(structType);

		return VmType(VM_TYPE_FUNCTION_REF, NULLC_PTR_SIZE + 4, structType);
	}

	static const VmType ArrayRef(TypeBase *structType)
	{
		assert(structType);

		return VmType(VM_TYPE_ARRAY_REF, NULLC_PTR_SIZE + 4, structType);
	}

	static const VmType Struct(long long size, TypeBase *structType)
	{
		assert(unsigned(size) == size);
		assert(structType);

		return VmType(VM_TYPE_STRUCT, unsigned(size), structType);
	}
};

struct VmValue
{
	VmValue(unsigned typeID, Allocator *allocator, VmType type): typeID(typeID), type(type), users(allocator)
	{
		hasSideEffects = false;
		hasMemoryAccess = false;

		canBeRemoved = true;
	}

	virtual ~VmValue()
	{
	}

	void AddUse(VmValue* user);
	void RemoveUse(VmValue* user);

	unsigned typeID;

	VmType type;

	InplaceStr comment;

	bool hasSideEffects;
	bool hasMemoryAccess;

	bool canBeRemoved;

	SmallArray<VmValue*, 8> users;
};

struct VmVoid: VmValue
{
	VmVoid(Allocator *allocator): VmValue(myTypeID, allocator, VmType::Void)
	{
	}

	static const unsigned myTypeID = __LINE__;
};

struct VmConstant: VmValue
{
	VmConstant(Allocator *allocator, VmType type): VmValue(myTypeID, allocator, type)
	{
		iValue = 0;
		dValue = 0.0;
		lValue = 0ll;
		sValue = NULL;

		container = NULL;
	}

	bool operator==(const VmConstant& rhs) const
	{
		return type == rhs.type && iValue == rhs.iValue && dValue == rhs.dValue && lValue == rhs.lValue && sValue == rhs.sValue && container == rhs.container;
	}

	int iValue;
	double dValue;
	long long lValue;
	const char *sValue;

	VariableData *container;

	static const unsigned myTypeID = __LINE__;
};

struct VmInstruction: VmValue
{
	VmInstruction(Allocator *allocator, VmType type, VmInstructionType cmd, unsigned uniqueId): VmValue(myTypeID, allocator, type), cmd(cmd), uniqueId(uniqueId), arguments(allocator)
	{
		parent = NULL;

		prevSibling = NULL;
		nextSibling = NULL;
	}

	void AddArgument(VmValue *argument);

	VmInstructionType cmd;

	unsigned uniqueId;

	SmallArray<VmValue*, 8> arguments;

	VmBlock *parent;

	VmInstruction *prevSibling;
	VmInstruction *nextSibling;

	static const unsigned myTypeID = __LINE__;
};

struct VmBlock: VmValue
{
	VmBlock(Allocator *allocator, InplaceStr name, unsigned uniqueId): VmValue(myTypeID, allocator, VmType::Label), name(name), uniqueId(uniqueId)
	{
		parent = NULL;

		prevSibling = NULL;
		nextSibling = NULL;

		firstInstruction = NULL;
		lastInstruction = NULL;

		insertPoint = NULL;

		address = ~0u;
	}

	void AddInstruction(VmInstruction* instruction);
	void RemoveInstruction(VmInstruction* instruction);

	InplaceStr name;

	unsigned uniqueId;

	VmFunction *parent;

	VmBlock *prevSibling;
	VmBlock *nextSibling;

	VmInstruction *firstInstruction;
	VmInstruction *lastInstruction;

	VmInstruction *insertPoint;

	unsigned address;

	static const unsigned myTypeID = __LINE__;
};

struct VmFunction: VmValue
{
	VmFunction(Allocator *allocator, VmType type, FunctionData *function, ScopeData *scope, VmType returnType): VmValue(myTypeID, allocator, type), function(function), scope(scope), returnType(returnType), allocas(allocator)
	{
		firstBlock = NULL;
		lastBlock = NULL;

		next = NULL;
		listed = false;

		address = ~0u;
	}

	void AddBlock(VmBlock *block);
	void RemoveBlock(VmBlock *block);

	FunctionData *function;
	ScopeData *scope;

	VmType returnType;

	VmBlock *firstBlock;
	VmBlock *lastBlock;

	VmFunction *next;
	bool listed;

	unsigned address;

	SmallArray<VariableData*, 4> allocas;

	static const unsigned myTypeID = __LINE__;
};

struct VmModule
{
	VmModule(Allocator *allocator): allocator(allocator), loopInfo(allocator), loadStoreInfo(allocator)
	{
		skipFunctionDefinitions = false;

		currentFunction = NULL;
		currentBlock = NULL;

		nextBlockId = 1;
		nextInstructionId = 1;

		peepholeOptimizations = 0;
		constantPropagations = 0;
		deadCodeEliminations = 0;
		controlFlowSimplifications = 0;
		loadStorePropagations = 0;
		commonSubexprEliminations = 0;
	}

	IntrusiveList<VmFunction> functions;

	bool skipFunctionDefinitions;

	VmFunction *currentFunction;
	VmBlock *currentBlock;

	unsigned nextBlockId;
	unsigned nextInstructionId;

	struct LoopInfo
	{
		LoopInfo(): breakBlock(0), continueBlock(0)
		{
		}

		LoopInfo(VmBlock* breakBlock, VmBlock* continueBlock): breakBlock(breakBlock), continueBlock(continueBlock)
		{
		}

		VmBlock* breakBlock;
		VmBlock* continueBlock;
	};

	SmallArray<LoopInfo, 32> loopInfo;

	unsigned peepholeOptimizations;
	unsigned constantPropagations;
	unsigned deadCodeEliminations;
	unsigned controlFlowSimplifications;
	unsigned loadStorePropagations;
	unsigned commonSubexprEliminations;

	struct LoadStoreInfo
	{
		LoadStoreInfo()
		{
			loadInst = 0;
			storeInst = 0;

			address = 0;
			pointer = 0;
		}

		VmInstruction *loadInst;
		VmInstruction *storeInst;

		VmConstant *address;
		VmValue *pointer;
	};

	SmallArray<LoadStoreInfo, 32> loadStoreInfo;

	// Memory pool
	Allocator *allocator;

	template<typename T>
	T* get()
	{
		return (T*)allocator->alloc(sizeof(T));
	}
};

template<typename T>
bool isType(VmValue *node)
{
	return node->typeID == typename T::myTypeID;
}

template<typename T>
T* getType(VmValue *node)
{
	if(node && isType<T>(node))
		return static_cast<T*>(node);

	return 0;
}

VmType GetVmType(ExpressionContext &ctx, TypeBase *type);

VmValue* CompileVm(ExpressionContext &ctx, VmModule *module, ExprBase *expression);
VmModule* CompileVm(ExpressionContext &ctx, ExprBase *expression);

void RunVmPass(ExpressionContext &ctx, VmModule *module, VmPassType type);