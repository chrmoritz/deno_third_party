// Copyright 2017 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/interpreter/interpreter-intrinsics-generator.h"

#include "src/builtins/builtins.h"
#include "src/codegen/code-factory.h"
#include "src/execution/frames.h"
#include "src/heap/factory-inl.h"
#include "src/interpreter/bytecodes.h"
#include "src/interpreter/interpreter-assembler.h"
#include "src/interpreter/interpreter-intrinsics.h"
#include "src/objects/js-generator.h"
#include "src/objects/objects-inl.h"
#include "src/objects/source-text-module.h"
#include "src/utils/allocation.h"

namespace v8 {
namespace internal {
namespace interpreter {

using compiler::Node;
template <typename T>
using TNode = compiler::TNode<T>;

class IntrinsicsGenerator {
 public:
  explicit IntrinsicsGenerator(InterpreterAssembler* assembler)
      : isolate_(assembler->isolate()),
        zone_(assembler->zone()),
        assembler_(assembler) {}

  Node* InvokeIntrinsic(TNode<Uint32T> function_id, TNode<Context> context,
                        const InterpreterAssembler::RegListNodePair& args);

 private:
  enum InstanceTypeCompareMode {
    kInstanceTypeEqual,
    kInstanceTypeGreaterThanOrEqual
  };

  Node* IsInstanceType(Node* input, int type);
  Node* CompareInstanceType(Node* map, int type, InstanceTypeCompareMode mode);
  Node* IntrinsicAsStubCall(const InterpreterAssembler::RegListNodePair& args,
                            TNode<Context> context, Callable const& callable);
  Node* IntrinsicAsBuiltinCall(
      const InterpreterAssembler::RegListNodePair& args, TNode<Context> context,
      Builtins::Name name);
  void AbortIfArgCountMismatch(int expected, compiler::TNode<Word32T> actual);

#define DECLARE_INTRINSIC_HELPER(name, lower_case, count)       \
  Node* name(const InterpreterAssembler::RegListNodePair& args, \
             TNode<Context> context);
  INTRINSICS_LIST(DECLARE_INTRINSIC_HELPER)
#undef DECLARE_INTRINSIC_HELPER

  Isolate* isolate() { return isolate_; }
  Zone* zone() { return zone_; }
  Factory* factory() { return isolate()->factory(); }

  Isolate* isolate_;
  Zone* zone_;
  InterpreterAssembler* assembler_;

  DISALLOW_COPY_AND_ASSIGN(IntrinsicsGenerator);
};

Node* GenerateInvokeIntrinsic(
    InterpreterAssembler* assembler, TNode<Uint32T> function_id,
    TNode<Context> context, const InterpreterAssembler::RegListNodePair& args) {
  IntrinsicsGenerator generator(assembler);
  return generator.InvokeIntrinsic(function_id, context, args);
}

#define __ assembler_->

Node* IntrinsicsGenerator::InvokeIntrinsic(
    TNode<Uint32T> function_id, TNode<Context> context,
    const InterpreterAssembler::RegListNodePair& args) {
  InterpreterAssembler::Label abort(assembler_), end(assembler_);
  InterpreterAssembler::Variable result(assembler_,
                                        MachineRepresentation::kTagged);

#define MAKE_LABEL(name, lower_case, count) \
  InterpreterAssembler::Label lower_case(assembler_);
  INTRINSICS_LIST(MAKE_LABEL)
#undef MAKE_LABEL

#define LABEL_POINTER(name, lower_case, count) &lower_case,
  InterpreterAssembler::Label* labels[] = {INTRINSICS_LIST(LABEL_POINTER)};
#undef LABEL_POINTER

#define CASE(name, lower_case, count) \
  static_cast<int32_t>(IntrinsicsHelper::IntrinsicId::k##name),
  int32_t cases[] = {INTRINSICS_LIST(CASE)};
#undef CASE

  __ Switch(function_id, &abort, cases, labels, arraysize(cases));
#define HANDLE_CASE(name, lower_case, expected_arg_count)            \
  __ BIND(&lower_case);                                              \
  {                                                                  \
    if (FLAG_debug_code && expected_arg_count >= 0) {                \
      AbortIfArgCountMismatch(expected_arg_count, args.reg_count()); \
    }                                                                \
    Node* value = name(args, context);                               \
    if (value) {                                                     \
      result.Bind(value);                                            \
      __ Goto(&end);                                                 \
    }                                                                \
  }
  INTRINSICS_LIST(HANDLE_CASE)
#undef HANDLE_CASE

  __ BIND(&abort);
  {
    __ Abort(AbortReason::kUnexpectedFunctionIDForInvokeIntrinsic);
    result.Bind(__ UndefinedConstant());
    __ Goto(&end);
  }

  __ BIND(&end);
  return result.value();
}

Node* IntrinsicsGenerator::CompareInstanceType(Node* object, int type,
                                               InstanceTypeCompareMode mode) {
  TNode<Uint16T> instance_type = __ LoadInstanceType(object);

  if (mode == kInstanceTypeEqual) {
    return __ Word32Equal(instance_type, __ Int32Constant(type));
  } else {
    DCHECK_EQ(mode, kInstanceTypeGreaterThanOrEqual);
    return __ Int32GreaterThanOrEqual(instance_type, __ Int32Constant(type));
  }
}

Node* IntrinsicsGenerator::IsInstanceType(Node* input, int type) {
  TNode<Oddball> result = __ Select<Oddball>(
      __ TaggedIsSmi(input), [=] { return __ FalseConstant(); },
      [=] {
        return __ SelectBooleanConstant(
            CompareInstanceType(input, type, kInstanceTypeEqual));
      });
  return result;
}

Node* IntrinsicsGenerator::IsJSReceiver(
    const InterpreterAssembler::RegListNodePair& args, TNode<Context> context) {
  TNode<Object> input = __ LoadRegisterFromRegisterList(args, 0);
  TNode<Oddball> result = __ Select<Oddball>(
      __ TaggedIsSmi(input), [=] { return __ FalseConstant(); },
      [=] {
        return __ SelectBooleanConstant(__ IsJSReceiver(__ CAST(input)));
      });
  return result;
}

Node* IntrinsicsGenerator::IsArray(
    const InterpreterAssembler::RegListNodePair& args, TNode<Context> context) {
  TNode<Object> input = __ LoadRegisterFromRegisterList(args, 0);
  return IsInstanceType(input, JS_ARRAY_TYPE);
}

Node* IntrinsicsGenerator::IsSmi(
    const InterpreterAssembler::RegListNodePair& args, TNode<Context> context) {
  TNode<Object> input = __ LoadRegisterFromRegisterList(args, 0);
  return __ SelectBooleanConstant(__ TaggedIsSmi(input));
}

Node* IntrinsicsGenerator::IntrinsicAsStubCall(
    const InterpreterAssembler::RegListNodePair& args, TNode<Context> context,
    Callable const& callable) {
  int param_count = callable.descriptor().GetParameterCount();
  int input_count = param_count + 2;  // +2 for target and context
  Node** stub_args = zone()->NewArray<Node*>(input_count);
  int index = 0;
  stub_args[index++] = __ HeapConstant(callable.code());
  for (int i = 0; i < param_count; i++) {
    stub_args[index++] = __ LoadRegisterFromRegisterList(args, i);
  }
  stub_args[index++] = context;
  return __ CallStubN(StubCallMode::kCallCodeObject, callable.descriptor(), 1,
                      input_count, stub_args);
}

Node* IntrinsicsGenerator::IntrinsicAsBuiltinCall(
    const InterpreterAssembler::RegListNodePair& args, TNode<Context> context,
    Builtins::Name name) {
  Callable callable = Builtins::CallableFor(isolate_, name);
  return IntrinsicAsStubCall(args, context, callable);
}

Node* IntrinsicsGenerator::CopyDataProperties(
    const InterpreterAssembler::RegListNodePair& args, TNode<Context> context) {
  return IntrinsicAsStubCall(
      args, context,
      Builtins::CallableFor(isolate(), Builtins::kCopyDataProperties));
}

Node* IntrinsicsGenerator::CreateIterResultObject(
    const InterpreterAssembler::RegListNodePair& args, TNode<Context> context) {
  return IntrinsicAsStubCall(
      args, context,
      Builtins::CallableFor(isolate(), Builtins::kCreateIterResultObject));
}

Node* IntrinsicsGenerator::HasProperty(
    const InterpreterAssembler::RegListNodePair& args, TNode<Context> context) {
  return IntrinsicAsStubCall(
      args, context, Builtins::CallableFor(isolate(), Builtins::kHasProperty));
}

Node* IntrinsicsGenerator::ToStringRT(
    const InterpreterAssembler::RegListNodePair& args, TNode<Context> context) {
  return IntrinsicAsStubCall(
      args, context, Builtins::CallableFor(isolate(), Builtins::kToString));
}

Node* IntrinsicsGenerator::ToLength(
    const InterpreterAssembler::RegListNodePair& args, TNode<Context> context) {
  return IntrinsicAsStubCall(
      args, context, Builtins::CallableFor(isolate(), Builtins::kToLength));
}

Node* IntrinsicsGenerator::ToObject(
    const InterpreterAssembler::RegListNodePair& args, TNode<Context> context) {
  return IntrinsicAsStubCall(
      args, context, Builtins::CallableFor(isolate(), Builtins::kToObject));
}

Node* IntrinsicsGenerator::Call(
    const InterpreterAssembler::RegListNodePair& args, TNode<Context> context) {
  // First argument register contains the function target.
  TNode<Object> function = __ LoadRegisterFromRegisterList(args, 0);

  // The arguments for the target function are from the second runtime call
  // argument.
  InterpreterAssembler::RegListNodePair target_args(
      __ RegisterLocationInRegisterList(args, 1),
      __ Int32Sub(args.reg_count(), __ Int32Constant(1)));

  if (FLAG_debug_code) {
    InterpreterAssembler::Label arg_count_positive(assembler_);
    TNode<BoolT> comparison =
        __ Int32LessThan(target_args.reg_count(), __ Int32Constant(0));
    __ GotoIfNot(comparison, &arg_count_positive);
    __ Abort(AbortReason::kWrongArgumentCountForInvokeIntrinsic);
    __ Goto(&arg_count_positive);
    __ BIND(&arg_count_positive);
  }

  __ CallJSAndDispatch(function, context, target_args,
                       ConvertReceiverMode::kAny);
  return nullptr;  // We never return from the CallJSAndDispatch above.
}

Node* IntrinsicsGenerator::CreateAsyncFromSyncIterator(
    const InterpreterAssembler::RegListNodePair& args, TNode<Context> context) {
  InterpreterAssembler::Label not_receiver(
      assembler_, InterpreterAssembler::Label::kDeferred);
  InterpreterAssembler::Label done(assembler_);
  InterpreterAssembler::Variable return_value(assembler_,
                                              MachineRepresentation::kTagged);

  TNode<Object> sync_iterator = __ LoadRegisterFromRegisterList(args, 0);

  __ GotoIf(__ TaggedIsSmi(sync_iterator), &not_receiver);
  __ GotoIfNot(__ IsJSReceiver(__ CAST(sync_iterator)), &not_receiver);

  TNode<Object> const next =
      __ GetProperty(context, sync_iterator, factory()->next_string());

  TNode<Context> const native_context = __ LoadNativeContext(context);
  TNode<Map> const map = __ CAST(__ LoadContextElement(
      native_context, Context::ASYNC_FROM_SYNC_ITERATOR_MAP_INDEX));
  TNode<JSObject> const iterator = __ AllocateJSObjectFromMap(map);

  __ StoreObjectFieldNoWriteBarrier(
      iterator, JSAsyncFromSyncIterator::kSyncIteratorOffset, sync_iterator);
  __ StoreObjectFieldNoWriteBarrier(iterator,
                                    JSAsyncFromSyncIterator::kNextOffset, next);

  return_value.Bind(iterator);
  __ Goto(&done);

  __ BIND(&not_receiver);
  {
    return_value.Bind(
        __ CallRuntime(Runtime::kThrowSymbolIteratorInvalid, context));

    // Unreachable due to the Throw in runtime call.
    __ Goto(&done);
  }

  __ BIND(&done);
  return return_value.value();
}

Node* IntrinsicsGenerator::CreateJSGeneratorObject(
    const InterpreterAssembler::RegListNodePair& args, TNode<Context> context) {
  return IntrinsicAsBuiltinCall(args, context,
                                Builtins::kCreateGeneratorObject);
}

Node* IntrinsicsGenerator::GeneratorGetResumeMode(
    const InterpreterAssembler::RegListNodePair& args, TNode<Context> context) {
  TNode<JSGeneratorObject> generator =
      __ CAST(__ LoadRegisterFromRegisterList(args, 0));
  TNode<Object> const value =
      __ LoadObjectField(generator, JSGeneratorObject::kResumeModeOffset);

  return value;
}

Node* IntrinsicsGenerator::GeneratorClose(
    const InterpreterAssembler::RegListNodePair& args, TNode<Context> context) {
  TNode<JSGeneratorObject> generator =
      __ CAST(__ LoadRegisterFromRegisterList(args, 0));
  __ StoreObjectFieldNoWriteBarrier(
      generator, JSGeneratorObject::kContinuationOffset,
      __ SmiConstant(JSGeneratorObject::kGeneratorClosed));
  return __ UndefinedConstant();
}

Node* IntrinsicsGenerator::GetImportMetaObject(
    const InterpreterAssembler::RegListNodePair& args, TNode<Context> context) {
  TNode<Context> const module_context = __ LoadModuleContext(context);
  TNode<HeapObject> const module =
      __ CAST(__ LoadContextElement(module_context, Context::EXTENSION_INDEX));
  TNode<Object> const import_meta =
      __ LoadObjectField(module, SourceTextModule::kImportMetaOffset);

  InterpreterAssembler::Variable return_value(assembler_,
                                              MachineRepresentation::kTagged);
  return_value.Bind(import_meta);

  InterpreterAssembler::Label end(assembler_);
  __ GotoIfNot(__ IsTheHole(import_meta), &end);

  return_value.Bind(__ CallRuntime(Runtime::kGetImportMetaObject, context));
  __ Goto(&end);

  __ BIND(&end);
  return return_value.value();
}

Node* IntrinsicsGenerator::AsyncFunctionAwaitCaught(
    const InterpreterAssembler::RegListNodePair& args, TNode<Context> context) {
  return IntrinsicAsBuiltinCall(args, context,
                                Builtins::kAsyncFunctionAwaitCaught);
}

Node* IntrinsicsGenerator::AsyncFunctionAwaitUncaught(
    const InterpreterAssembler::RegListNodePair& args, TNode<Context> context) {
  return IntrinsicAsBuiltinCall(args, context,
                                Builtins::kAsyncFunctionAwaitUncaught);
}

Node* IntrinsicsGenerator::AsyncFunctionEnter(
    const InterpreterAssembler::RegListNodePair& args, TNode<Context> context) {
  return IntrinsicAsBuiltinCall(args, context, Builtins::kAsyncFunctionEnter);
}

Node* IntrinsicsGenerator::AsyncFunctionReject(
    const InterpreterAssembler::RegListNodePair& args, TNode<Context> context) {
  return IntrinsicAsBuiltinCall(args, context, Builtins::kAsyncFunctionReject);
}

Node* IntrinsicsGenerator::AsyncFunctionResolve(
    const InterpreterAssembler::RegListNodePair& args, TNode<Context> context) {
  return IntrinsicAsBuiltinCall(args, context, Builtins::kAsyncFunctionResolve);
}

Node* IntrinsicsGenerator::AsyncGeneratorAwaitCaught(
    const InterpreterAssembler::RegListNodePair& args, TNode<Context> context) {
  return IntrinsicAsBuiltinCall(args, context,
                                Builtins::kAsyncGeneratorAwaitCaught);
}

Node* IntrinsicsGenerator::AsyncGeneratorAwaitUncaught(
    const InterpreterAssembler::RegListNodePair& args, TNode<Context> context) {
  return IntrinsicAsBuiltinCall(args, context,
                                Builtins::kAsyncGeneratorAwaitUncaught);
}

Node* IntrinsicsGenerator::AsyncGeneratorReject(
    const InterpreterAssembler::RegListNodePair& args, TNode<Context> context) {
  return IntrinsicAsBuiltinCall(args, context, Builtins::kAsyncGeneratorReject);
}

Node* IntrinsicsGenerator::AsyncGeneratorResolve(
    const InterpreterAssembler::RegListNodePair& args, TNode<Context> context) {
  return IntrinsicAsBuiltinCall(args, context,
                                Builtins::kAsyncGeneratorResolve);
}

Node* IntrinsicsGenerator::AsyncGeneratorYield(
    const InterpreterAssembler::RegListNodePair& args, TNode<Context> context) {
  return IntrinsicAsBuiltinCall(args, context, Builtins::kAsyncGeneratorYield);
}

void IntrinsicsGenerator::AbortIfArgCountMismatch(int expected,
                                                  TNode<Word32T> actual) {
  InterpreterAssembler::Label match(assembler_);
  TNode<BoolT> comparison = __ Word32Equal(actual, __ Int32Constant(expected));
  __ GotoIf(comparison, &match);
  __ Abort(AbortReason::kWrongArgumentCountForInvokeIntrinsic);
  __ Goto(&match);
  __ BIND(&match);
}

#undef __

}  // namespace interpreter
}  // namespace internal
}  // namespace v8
