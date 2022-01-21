// Copyright 2021 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "xls/dslx/bytecode_interpreter.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/container/flat_hash_map.h"
#include "xls/common/status/matchers.h"
#include "xls/dslx/bytecode_emitter.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/parse_and_typecheck.h"

namespace xls::dslx {
namespace {

using status_testing::StatusIs;
using testing::HasSubstr;

// Interprets a nearly-minimal bytecode program; the same from
// BytecodeEmitterTest.SimpleTranslation.
TEST(BytecodeInterpreterTest, PositiveSmokeTest) {
  std::vector<Bytecode> bytecodes;
  bytecodes.push_back(
      Bytecode(Span::Fake(), Bytecode::Op::kLiteral, InterpValue::MakeU32(1)));
  bytecodes.push_back(Bytecode(Span::Fake(), Bytecode::Op::kStore, 0));
  bytecodes.push_back(Bytecode(Span::Fake(), Bytecode::Op::kLoad, 0));
  bytecodes.push_back(
      Bytecode(Span::Fake(), Bytecode::Op::kLiteral, InterpValue::MakeU32(2)));
  bytecodes.push_back(Bytecode(Span::Fake(), Bytecode::Op::kAdd));

  std::vector<InterpValue> env;
  env.push_back(InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));
  EXPECT_EQ(value, InterpValue::MakeU32(3));
}

// Tests that a failing assert_eq is interpreted correctly. Again, a
// continuation of a test from BytecodeEmitterTest. Get used to it.
TEST(BytecodeInterpreterTest, AssertEqFail) {
  std::vector<Bytecode> bytecodes;
  bytecodes.push_back(
      Bytecode(Span::Fake(), Bytecode::Op::kLiteral, InterpValue::MakeU32(3)));
  bytecodes.push_back(Bytecode(Span::Fake(), Bytecode::Op::kStore, 0));
  bytecodes.push_back(Bytecode(Span::Fake(), Bytecode::Op::kLoad, 0));
  bytecodes.push_back(
      Bytecode(Span::Fake(), Bytecode::Op::kLiteral, InterpValue::MakeU32(2)));
  InterpValue fn_value(InterpValue::MakeFunction(Builtin::kAssertEq));
  bytecodes.push_back(Bytecode(Span::Fake(), Bytecode::Op::kCall, fn_value));
  bytecodes.push_back(Bytecode(Span::Fake(), Bytecode::Op::kStore, 1));
  bytecodes.push_back(Bytecode(Span::Fake(), Bytecode::Op::kLoad, 0));

  std::vector<InterpValue> env;
  env.push_back(InterpValue::MakeUnit());
  env.push_back(InterpValue::MakeUnit());
  absl::StatusOr<InterpValue> value =
      BytecodeInterpreter::Interpret(bytecodes, &env);
  EXPECT_THAT(value.status(), StatusIs(absl::StatusCode::kInternal,
                                       HasSubstr("were not equal")));
}

// This test won't work unless BytecodeEmitterTest.DestructuringLet works!
TEST(BytecodeInterpreterTest, DestructuringLet) {
  constexpr absl::string_view kProgram = R"(#![test]
fn has_name_def_tree() -> (u32, u64, uN[128]) {
  let (a, b, (c, d)) = (u4:0, u8:1, (u16:2, (u32:3, u64:4, uN[128]:5)));
  let _ = assert_eq(a, u4:0);
  let _ = assert_eq(b, u8:1);
  let _ = assert_eq(c, u16:2);
  let _ = assert_eq(d, (u32:3, u64:4, uN[128]:5));
  d
})";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf,
                           tm.module->GetTest("has_name_def_tree"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(8, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));

  ASSERT_TRUE(value.IsTuple());
  XLS_ASSERT_OK_AND_ASSIGN(int64_t num_elements, value.GetLength());
  ASSERT_EQ(num_elements, 3);
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue element,
                           value.Index(InterpValue::MakeU32(0)));
  XLS_ASSERT_OK_AND_ASSIGN(int64_t bit_value, element.GetBitValueInt64());
  EXPECT_EQ(bit_value, 3);

  XLS_ASSERT_OK_AND_ASSIGN(element, value.Index(InterpValue::MakeU32(1)));
  XLS_ASSERT_OK_AND_ASSIGN(bit_value, element.GetBitValueInt64());
  EXPECT_EQ(bit_value, 4);

  XLS_ASSERT_OK_AND_ASSIGN(element, value.Index(InterpValue::MakeU32(2)));
  XLS_ASSERT_OK_AND_ASSIGN(bit_value, element.GetBitValueInt64());
  EXPECT_EQ(bit_value, 5);
}

TEST(BytecodeInterpreterTest, RunTernaryConsequent) {
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           BytecodesFromString(
                               R"(000 literal u1:1
001 jump_rel_if +3
002 literal u32:64
003 jump_rel +3
004 jump_dest
005 literal u32:42
006 jump_dest)"));

  std::vector<InterpValue> env;
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));
  EXPECT_EQ(value, InterpValue::MakeU32(42)) << value.ToString();
}

TEST(BytecodeInterpreterTest, RunTernaryAlternate) {
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           BytecodesFromString(
                               R"(000 literal u1:0
001 jump_rel_if +3
002 literal u32:64
003 jump_rel +3
004 jump_dest
005 literal u32:42
006 jump_dest)"));

  std::vector<InterpValue> env;
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));
  EXPECT_EQ(value, InterpValue::MakeU32(64)) << value.ToString();
}

TEST(BytecodeInterpreterTest, BinopAnd) {
  constexpr absl::string_view kProgram = R"(#![test]
fn do_and() -> u32 {
  let a = u32:0xa5a5a5a5;
  let b = u32:0xffffffff;
  a & b
})";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf, tm.module->GetTest("do_and"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(2, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));

  XLS_ASSERT_OK_AND_ASSIGN(Bits bits, value.GetBits());
  XLS_ASSERT_OK_AND_ASSIGN(uint64_t int_val, bits.ToUint64());
  EXPECT_EQ(int_val, 0xa5a5a5a5ll);
}

TEST(BytecodeInterpreterTest, BinopConcat) {
  constexpr absl::string_view kProgram = R"(#![test]
fn do_concat() -> u64 {
  let a = u32:0xa5a5a5a5;
  let b = u32:0xffffffff;
  a ++ b
})";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf, tm.module->GetTest("do_concat"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(2, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));

  XLS_ASSERT_OK_AND_ASSIGN(Bits bits, value.GetBits());
  XLS_ASSERT_OK_AND_ASSIGN(uint64_t int_val, bits.ToUint64());
  EXPECT_EQ(int_val, 0xa5a5a5a5ffffffffll);
}

TEST(BytecodeInterpreterTest, BinopDiv) {
  constexpr absl::string_view kProgram = R"(#![test]
fn do_div() -> u32 {
  let a = u32:0x84208420;
  let b = u32:0x4;
  a / b
})";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf, tm.module->GetTest("do_div"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(2, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));

  XLS_ASSERT_OK_AND_ASSIGN(Bits bits, value.GetBits());
  XLS_ASSERT_OK_AND_ASSIGN(uint64_t int_val, bits.ToUint64());
  EXPECT_EQ(int_val, 0x21082108);
}

TEST(BytecodeInterpreterTest, BinopMul) {
  constexpr absl::string_view kProgram = R"(#![test]
fn do_mul() -> u32 {
  let a = u32:0x21082108;
  let b = u32:0x4;
  a * b
})";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf, tm.module->GetTest("do_mul"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(2, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));

  XLS_ASSERT_OK_AND_ASSIGN(Bits bits, value.GetBits());
  XLS_ASSERT_OK_AND_ASSIGN(uint64_t int_val, bits.ToUint64());
  EXPECT_EQ(int_val, 0x84208420);
}

TEST(BytecodeInterpreterTest, BinopOr) {
  constexpr absl::string_view kProgram = R"(#![test]
fn do_or() -> u32 {
  let a = u32:0xa5a5a5a5;
  let b = u32:0x5a5a5a5a;
  a | b
})";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf, tm.module->GetTest("do_or"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(2, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));

  XLS_ASSERT_OK_AND_ASSIGN(Bits bits, value.GetBits());
  XLS_ASSERT_OK_AND_ASSIGN(uint64_t int_val, bits.ToUint64());
  EXPECT_EQ(int_val, 0xffffffff);
}

TEST(BytecodeInterpreterTest, BinopShll) {
  constexpr absl::string_view kProgram = R"(#![test]
fn do_shll() -> u32 {
  let a = u32:0x21082108;
  let b = u32:0x2;
  a << b
})";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf, tm.module->GetTest("do_shll"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(2, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));

  XLS_ASSERT_OK_AND_ASSIGN(Bits bits, value.GetBits());
  XLS_ASSERT_OK_AND_ASSIGN(uint64_t int_val, bits.ToUint64());
  EXPECT_EQ(int_val, 0x84208420);
}

TEST(BytecodeInterpreterTest, BinopShrl) {
  constexpr absl::string_view kProgram = R"(#![test]
fn do_shrl() -> u32 {
  let a = u32:0x84208420;
  let b = u32:0x2;
  a >> b
})";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf, tm.module->GetTest("do_shrl"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(2, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));

  XLS_ASSERT_OK_AND_ASSIGN(Bits bits, value.GetBits());
  XLS_ASSERT_OK_AND_ASSIGN(uint64_t int_val, bits.ToUint64());
  EXPECT_EQ(int_val, 0x21082108);
}

TEST(BytecodeInterpreterTest, BinopSub) {
  constexpr absl::string_view kProgram = R"(#![test]
fn do_sub() -> u32 {
  let a = u32:0xa5a5a5a5;
  let b = u32:0x5a5a5a5a;
  a - b
})";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf, tm.module->GetTest("do_sub"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(2, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));

  XLS_ASSERT_OK_AND_ASSIGN(Bits bits, value.GetBits());
  XLS_ASSERT_OK_AND_ASSIGN(uint64_t int_val, bits.ToUint64());
  EXPECT_EQ(int_val, 0x4b4b4b4b);
}

TEST(BytecodeInterpreterTest, BinopXor) {
  constexpr absl::string_view kProgram = R"(#![test]
fn do_xor() -> u32 {
  let a = u32:0xa5a5ffff;
  let b = u32:0x5a5affff;
  a ^ b
})";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf, tm.module->GetTest("do_xor"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(2, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));

  XLS_ASSERT_OK_AND_ASSIGN(Bits bits, value.GetBits());
  XLS_ASSERT_OK_AND_ASSIGN(uint64_t int_val, bits.ToUint64());
  EXPECT_EQ(int_val, 0xffff0000);
}

TEST(BytecodeInterpreterTest, Unops) {
  constexpr absl::string_view kProgram = R"(#![test]
fn unops() -> s32 {
  let a = s32:1;
  let b = !a;
  -b
})";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf, tm.module->GetTest("unops"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(2, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));

  XLS_ASSERT_OK_AND_ASSIGN(Bits bits, value.GetBits());
  XLS_ASSERT_OK_AND_ASSIGN(uint64_t int_val, bits.ToUint64());
  EXPECT_EQ(int_val, 0x2);
}

TEST(BytecodeInterpreterTest, CreateArray) {
  constexpr absl::string_view kProgram = R"(#![test]
fn arrays() -> u32[3] {
  let a = u32:32;
  u32[3]:[u32:0, u32:1, a]
}
)";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf, tm.module->GetTest("arrays"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(2, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));
  ASSERT_TRUE(value.IsArray());
  XLS_ASSERT_OK_AND_ASSIGN(int64_t num_elements, value.GetLength());
  ASSERT_EQ(num_elements, 3);
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue element,
                           value.Index(InterpValue::MakeU32(0)));
  XLS_ASSERT_OK_AND_ASSIGN(int64_t bit_value, element.GetBitValueInt64());
  EXPECT_EQ(bit_value, 0);

  XLS_ASSERT_OK_AND_ASSIGN(element, value.Index(InterpValue::MakeU32(1)));
  XLS_ASSERT_OK_AND_ASSIGN(bit_value, element.GetBitValueInt64());
  EXPECT_EQ(bit_value, 1);

  XLS_ASSERT_OK_AND_ASSIGN(element, value.Index(InterpValue::MakeU32(2)));
  XLS_ASSERT_OK_AND_ASSIGN(bit_value, element.GetBitValueInt64());
  EXPECT_EQ(bit_value, 32);
}

TEST(BytecodeInterpreterTest, IndexArray) {
  constexpr absl::string_view kProgram = R"(#![test]
fn index_array() -> u32 {
  let a = u32[3]:[0, 1, 2];
  let b = bits[32][3]:[3, 4, 5];

  a[u32:0] + b[u32:1]
})";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf,
                           tm.module->GetTest("index_array"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(2, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));
  ASSERT_TRUE(value.IsBits());
  XLS_ASSERT_OK_AND_ASSIGN(int64_t int_value, value.GetBitsOrDie().ToInt64());
  EXPECT_EQ(int_value, 4);
}

TEST(BytecodeInterpreterTest, IndexTuple) {
  constexpr absl::string_view kProgram = R"(#![test]
fn index_tuple() -> u32 {
  let a = (u32:0, (u32:1, u32:2));
  let b = ((u32:3, (u32:4,)), u32:5);

  a[1][1] + b[0][1][0]
})";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf,
                           tm.module->GetTest("index_tuple"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(2, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));
  ASSERT_TRUE(value.IsBits());
  XLS_ASSERT_OK_AND_ASSIGN(int64_t int_value, value.GetBitsOrDie().ToInt64());
  EXPECT_EQ(int_value, 6);
}

TEST(BytecodeInterpreterTest, SimpleBitSlice) {
  constexpr absl::string_view kProgram = R"(#![test]
fn simple_slice() -> u16 {
  let a = u32:0xdeadbeef;
  a[16:32]
}
)";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf,
                           tm.module->GetTest("simple_slice"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(2, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));
  ASSERT_TRUE(value.IsBits());
  XLS_ASSERT_OK_AND_ASSIGN(int64_t int_value, value.GetBitsOrDie().ToUint64());
  EXPECT_EQ(int_value, 0xdead);
}

// Tests a slice from the start: a[-x:].
TEST(BytecodeInterpreterTest, NegativeStartSlice) {
  constexpr absl::string_view kProgram = R"(#![test]
fn negative_start_slice() -> u16 {
  let a = u32:0xdeadbeef;
  a[-16:]
}
)";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf,
                           tm.module->GetTest("negative_start_slice"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(2, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));
  ASSERT_TRUE(value.IsBits());
  XLS_ASSERT_OK_AND_ASSIGN(int64_t int_value, value.GetBitsOrDie().ToUint64());
  EXPECT_EQ(int_value, 0xdead);
}

// Tests a slice from the end: a[:-x].
TEST(BytecodeInterpreterTest, NegativeEndSlice) {
  constexpr absl::string_view kProgram = R"(#![test]
fn negative_end_slice() -> u16 {
  let a = u32:0xdeadbeef;
  a[:-16]
}
)";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf,
                           tm.module->GetTest("negative_end_slice"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(2, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));
  ASSERT_TRUE(value.IsBits());
  XLS_ASSERT_OK_AND_ASSIGN(int64_t int_value, value.GetBitsOrDie().ToUint64());
  EXPECT_EQ(int_value, 0xbeef);
}

// Tests a slice from both ends: a[-x:-y].
TEST(BytecodeInterpreterTest, BothNegativeSlice) {
  constexpr absl::string_view kProgram = R"(#![test]
fn both_negative_slice() -> u8 {
  let a = u32:0xdeadbeef;
  a[-16:-8]
}
)";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf,
                           tm.module->GetTest("both_negative_slice"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(2, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));
  ASSERT_TRUE(value.IsBits());
  XLS_ASSERT_OK_AND_ASSIGN(int64_t int_value, value.GetBitsOrDie().ToUint64());
  EXPECT_EQ(int_value, 0xad);
}

TEST(BytecodeInterpreterTest, CastBits_Extend) {
  constexpr absl::string_view kProgram = R"(#![test]
fn cast_extend() -> u32 {
  let a = u16:0xa5a5;
  let b = s16:0x8000;
  a as u32 + ((b as s32) as u32)
}
)";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf,
                           tm.module->GetTest("cast_extend"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(2, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));

  XLS_ASSERT_OK_AND_ASSIGN(Bits bits, value.GetBits());
  XLS_ASSERT_OK_AND_ASSIGN(uint64_t int_val, bits.ToUint64());
  EXPECT_EQ(int_val, 0x25a5);
}

TEST(BytecodeInterpreterTest, CastBits_Shrink) {
  constexpr absl::string_view kProgram = R"(#![test]
fn cast_shrink() -> u16 {
  let a = u32:0x0000a5a5;
  let b = s32:0x8fff5a5a;
  a as u16 + b as u16
})";
  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf,
                           tm.module->GetTest("cast_shrink"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(2, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));

  XLS_ASSERT_OK_AND_ASSIGN(Bits bits, value.GetBits());
  XLS_ASSERT_OK_AND_ASSIGN(uint64_t int_val, bits.ToUint64());
  EXPECT_EQ(int_val, 0xffff);
}

TEST(BytecodeInterpreterTest, CastArrayToBits) {
  constexpr absl::string_view kProgram = R"(#![test]
fn cast_array_to_bits() -> u32 {
  let a = u8[4]:[0xc, 0xa, 0xf, 0xe];
  a as u32
})";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf,
                           tm.module->GetTest("cast_array_to_bits"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(2, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));

  XLS_ASSERT_OK_AND_ASSIGN(Bits bits, value.GetBits());
  XLS_ASSERT_OK_AND_ASSIGN(uint64_t int_val, bits.ToUint64());
  EXPECT_EQ(int_val, 0x0c0a0f0e);
}

TEST(BytecodeInterpreterTest, CastBitsToArray) {
  constexpr absl::string_view kProgram = R"(#![test]
fn cast_bits_to_array() -> u8 {
  let a = u32:0x0c0a0f0e;
  let b = a as u8[4];
  b[u32:2]
})";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf,
                           tm.module->GetTest("cast_bits_to_array"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(2, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));

  XLS_ASSERT_OK_AND_ASSIGN(Bits bits, value.GetBits());
  XLS_ASSERT_OK_AND_ASSIGN(uint64_t int_val, bits.ToUint64());
  EXPECT_EQ(int_val, 0x0f);
}

TEST(BytecodeInterpreterTest, CastEnumToBits) {
  constexpr absl::string_view kProgram = R"(enum MyEnum : u3 {
  VAL_0 = 0,
  VAL_1 = 1,
  VAL_2 = 2,
  VAL_3 = 3,
}

#![test]
fn cast_enum_to_bits() -> u3 {
  let a = MyEnum::VAL_3;
  a as u3
})";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf,
                           tm.module->GetTest("cast_enum_to_bits"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(2, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));

  XLS_ASSERT_OK_AND_ASSIGN(Bits bits, value.GetBits());
  XLS_ASSERT_OK_AND_ASSIGN(uint64_t int_val, bits.ToUint64());
  EXPECT_EQ(int_val, 3);
}

TEST(BytecodeInterpreterTest, CastBitsToEnum) {
  constexpr absl::string_view kProgram = R"(enum MyEnum : u3 {
  VAL_0 = 0,
  VAL_1 = 1,
  VAL_2 = 2,
  VAL_3 = 3,
}

#![test]
fn cast_bits_to_enum() -> MyEnum {
  let a = u3:2;
  a as MyEnum
})";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf,
                           tm.module->GetTest("cast_bits_to_enum"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));

  std::vector<InterpValue> env(2, InterpValue::MakeUnit());
  XLS_ASSERT_OK_AND_ASSIGN(InterpValue value,
                           BytecodeInterpreter::Interpret(bytecodes, &env));

  XLS_ASSERT_OK_AND_ASSIGN(Bits bits, value.GetBits());
  XLS_ASSERT_OK_AND_ASSIGN(uint64_t int_val, bits.ToUint64());
  EXPECT_EQ(int_val, 2);
}

TEST(BytecodeInterpreterTest, CastWithMissingData) {
  constexpr absl::string_view kProgram = R"(enum MyEnum : u3 {
  VAL_0 = 0,
  VAL_1 = 1,
  VAL_2 = 2,
  VAL_3 = 3,
}

#![test]
fn cast_bits_to_enum() -> MyEnum {
  let a = u3:2;
  a as MyEnum
})";

  auto import_data = ImportData::CreateForTest();
  XLS_ASSERT_OK_AND_ASSIGN(
      TypecheckedModule tm,
      ParseAndTypecheck(kProgram, "test.x", "test", &import_data));

  absl::flat_hash_map<const NameDef*, int64_t> namedef_to_slot;
  BytecodeEmitter emitter(&import_data, tm.type_info, &namedef_to_slot);
  XLS_ASSERT_OK_AND_ASSIGN(TestFunction * tf,
                           tm.module->GetTest("cast_bits_to_enum"));
  XLS_ASSERT_OK_AND_ASSIGN(std::vector<Bytecode> bytecodes,
                           emitter.Emit(tf->fn()));
  // Clear out the data element of the bytecode, the cast op.
  bytecodes[bytecodes.size() - 1] = Bytecode(Span::Fake(), Bytecode::Op::kCast);

  std::vector<InterpValue> env(2, InterpValue::MakeUnit());
  absl::StatusOr<InterpValue> value =
      BytecodeInterpreter::Interpret(bytecodes, &env);
  EXPECT_THAT(value.status(),
              StatusIs(absl::StatusCode::kInternal,
                       HasSubstr("Cast op requires ConcreteType data.")));
}

}  // namespace
}  // namespace xls::dslx
