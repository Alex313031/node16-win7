// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/regexp/experimental/experimental-compiler.h"

#include "src/base/strings.h"
#include "src/regexp/experimental/experimental.h"
#include "src/zone/zone-list-inl.h"

namespace v8 {
namespace internal {

namespace {

// TODO(mbid, v8:10765): Currently the experimental engine doesn't support
// UTF-16, but this shouldn't be too hard to implement.
constexpr base::uc32 kMaxSupportedCodepoint = 0xFFFFu;

class CanBeHandledVisitor final : private RegExpVisitor {
  // Visitor to implement `ExperimentalRegExp::CanBeHandled`.
 public:
  static bool Check(RegExpTree* tree, JSRegExp::Flags flags,
                    int capture_count) {
    if (!AreSuitableFlags(flags)) return false;
    CanBeHandledVisitor visitor;
    tree->Accept(&visitor, nullptr);
    return visitor.result_;
  }

 private:
  CanBeHandledVisitor() = default;

  static bool AreSuitableFlags(JSRegExp::Flags flags) {
    // TODO(mbid, v8:10765): We should be able to support all flags in the
    // future.
    static constexpr JSRegExp::Flags kAllowedFlags =
        JSRegExp::kGlobal | JSRegExp::kSticky | JSRegExp::kMultiline |
        JSRegExp::kDotAll | JSRegExp::kLinear;
    // We support Unicode iff kUnicode is among the supported flags.
    STATIC_ASSERT(ExperimentalRegExp::kSupportsUnicode ==
                  ((kAllowedFlags & JSRegExp::kUnicode) != 0));
    return (flags & ~kAllowedFlags) == 0;
  }

  void* VisitDisjunction(RegExpDisjunction* node, void*) override {
    for (RegExpTree* alt : *node->alternatives()) {
      alt->Accept(this, nullptr);
      if (!result_) {
        return nullptr;
      }
    }
    return nullptr;
  }

  void* VisitAlternative(RegExpAlternative* node, void*) override {
    for (RegExpTree* child : *node->nodes()) {
      child->Accept(this, nullptr);
      if (!result_) {
        return nullptr;
      }
    }
    return nullptr;
  }

  void* VisitCharacterClass(RegExpCharacterClass* node, void*) override {
    return nullptr;
  }

  void* VisitAssertion(RegExpAssertion* node, void*) override {
    return nullptr;
  }

  void* VisitAtom(RegExpAtom* node, void*) override {
    return nullptr;
  }

  void* VisitText(RegExpText* node, void*) override {
    for (TextElement& el : *node->elements()) {
      el.tree()->Accept(this, nullptr);
      if (!result_) {
        return nullptr;
      }
    }
    return nullptr;
  }

  void* VisitQuantifier(RegExpQuantifier* node, void*) override {
    // Finite but large values of `min()` and `max()` are bad for the
    // breadth-first engine because finite (optional) repetition is dealt with
    // by replicating the bytecode of the body of the quantifier.  The number
    // of replicatons grows exponentially in how deeply quantifiers are nested.
    // `replication_factor_` keeps track of how often the current node will
    // have to be replicated in the generated bytecode, and we don't allow this
    // to exceed some small value.
    static constexpr int kMaxReplicationFactor = 16;

    // First we rule out values for min and max that are too big even before
    // taking into account the ambient replication_factor_.  This also guards
    // against overflows in `local_replication` or `replication_factor_`.
    if (node->min() > kMaxReplicationFactor ||
        (node->max() != RegExpTree::kInfinity &&
         node->max() > kMaxReplicationFactor)) {
      result_ = false;
      return nullptr;
    }

    // Save the current replication factor so that it can be restored if we
    // return with `result_ == true`.
    int before_replication_factor = replication_factor_;

    int local_replication;
    if (node->max() == RegExpTree::kInfinity) {
      local_replication = node->min() + 1;
    } else {
      local_replication = node->max();
    }

    replication_factor_ *= local_replication;
    if (replication_factor_ > kMaxReplicationFactor) {
      result_ = false;
      return nullptr;
    }

    switch (node->quantifier_type()) {
      case RegExpQuantifier::GREEDY:
      case RegExpQuantifier::NON_GREEDY:
        break;
      case RegExpQuantifier::POSSESSIVE:
        // TODO(mbid, v8:10765): It's not clear to me whether this can be
        // supported in breadth-first mode. Re2 doesn't support it.
        result_ = false;
        return nullptr;
    }

    node->body()->Accept(this, nullptr);
    replication_factor_ = before_replication_factor;
    return nullptr;
  }

  void* VisitCapture(RegExpCapture* node, void*) override {
    node->body()->Accept(this, nullptr);
    return nullptr;
  }

  void* VisitGroup(RegExpGroup* node, void*) override {
    node->body()->Accept(this, nullptr);
    return nullptr;
  }

  void* VisitLookaround(RegExpLookaround* node, void*) override {
    // TODO(mbid, v8:10765): This will be hard to support, but not impossible I
    // think.  See product automata.
    result_ = false;
    return nullptr;
  }

  void* VisitBackReference(RegExpBackReference* node, void*) override {
    // This can't be implemented without backtracking.
    result_ = false;
    return nullptr;
  }

  void* VisitEmpty(RegExpEmpty* node, void*) override { return nullptr; }

 private:
  // See comment in `VisitQuantifier`:
  int replication_factor_ = 1;

  bool result_ = true;
};

}  // namespace

bool ExperimentalRegExpCompiler::CanBeHandled(RegExpTree* tree,
                                              JSRegExp::Flags flags,
                                              int capture_count) {
  return CanBeHandledVisitor::Check(tree, flags, capture_count);
}

namespace {

// A label in bytecode which starts with no known address. The address *must*
// be bound with `Bind` before the label goes out of scope.
// Implemented as a linked list through the `payload.pc` of FORK and JMP
// instructions.
struct Label {
 public:
  Label() = default;
  ~Label() {
    DCHECK_EQ(state_, BOUND);
    DCHECK_GE(bound_index_, 0);
  }

  // Don't copy, don't move.  Moving could be implemented, but it's not
  // needed anywhere.
  Label(const Label&) = delete;
  Label& operator=(const Label&) = delete;

 private:
  friend class BytecodeAssembler;

  // UNBOUND implies unbound_patch_list_begin_.
  // BOUND implies bound_index_.
  enum { UNBOUND, BOUND } state_ = UNBOUND;
  union {
    int unbound_patch_list_begin_ = -1;
    int bound_index_;
  };
};

class BytecodeAssembler {
 public:
  // TODO(mbid,v8:10765): Use some upper bound for code_ capacity computed from
  // the `tree` size we're going to compile?
  explicit BytecodeAssembler(Zone* zone) : zone_(zone), code_(0, zone) {}

  ZoneList<RegExpInstruction> IntoCode() && { return std::move(code_); }

  void Accept() { code_.Add(RegExpInstruction::Accept(), zone_); }

  void Assertion(RegExpAssertion::AssertionType t) {
    code_.Add(RegExpInstruction::Assertion(t), zone_);
  }

  void ClearRegister(int32_t register_index) {
    code_.Add(RegExpInstruction::ClearRegister(register_index), zone_);
  }

  void ConsumeRange(base::uc16 from, base::uc16 to) {
    code_.Add(RegExpInstruction::ConsumeRange(from, to), zone_);
  }

  void ConsumeAnyChar() {
    code_.Add(RegExpInstruction::ConsumeAnyChar(), zone_);
  }

  void Fork(Label& target) {
    LabelledInstrImpl(RegExpInstruction::Opcode::FORK, target);
  }

  void Jmp(Label& target) {
    LabelledInstrImpl(RegExpInstruction::Opcode::JMP, target);
  }

  void SetRegisterToCp(int32_t register_index) {
    code_.Add(RegExpInstruction::SetRegisterToCp(register_index), zone_);
  }

  void Bind(Label& target) {
    DCHECK_EQ(target.state_, Label::UNBOUND);

    int index = code_.length();

    while (target.unbound_patch_list_begin_ != -1) {
      RegExpInstruction& inst = code_[target.unbound_patch_list_begin_];
      DCHECK(inst.opcode == RegExpInstruction::FORK ||
             inst.opcode == RegExpInstruction::JMP);

      target.unbound_patch_list_begin_ = inst.payload.pc;
      inst.payload.pc = index;
    }

    target.state_ = Label::BOUND;
    target.bound_index_ = index;
  }

  void Fail() { code_.Add(RegExpInstruction::Fail(), zone_); }

 private:
  void LabelledInstrImpl(RegExpInstruction::Opcode op, Label& target) {
    RegExpInstruction result;
    result.opcode = op;

    if (target.state_ == Label::BOUND) {
      result.payload.pc = target.bound_index_;
    } else {
      DCHECK_EQ(target.state_, Label::UNBOUND);
      int new_list_begin = code_.length();
      DCHECK_GE(new_list_begin, 0);

      result.payload.pc = target.unbound_patch_list_begin_;

      target.unbound_patch_list_begin_ = new_list_begin;
    }

    code_.Add(result, zone_);
  }

  Zone* zone_;
  ZoneList<RegExpInstruction> code_;
};

class CompileVisitor : private RegExpVisitor {
 public:
  static ZoneList<RegExpInstruction> Compile(RegExpTree* tree,
                                             JSRegExp::Flags flags,
                                             Zone* zone) {
    CompileVisitor compiler(zone);

    if ((flags & JSRegExp::kSticky) == 0 && !tree->IsAnchoredAtStart()) {
      // The match is not anchored, i.e. may start at any input position, so we
      // emit a preamble corresponding to /.*?/.  This skips an arbitrary
      // prefix in the input non-greedily.
      compiler.CompileNonGreedyStar(
          [&]() { compiler.assembler_.ConsumeAnyChar(); });
    }

    compiler.assembler_.SetRegisterToCp(0);
    tree->Accept(&compiler, nullptr);
    compiler.assembler_.SetRegisterToCp(1);
    compiler.assembler_.Accept();

    return std::move(compiler.assembler_).IntoCode();
  }

 private:
  explicit CompileVisitor(Zone* zone) : zone_(zone), assembler_(zone) {}

  // Generate a disjunction of code fragments compiled by a function `alt_gen`.
  // `alt_gen` is called repeatedly with argument `int i = 0, 1, ..., alt_num -
  // 1` and should build code corresponding to the ith alternative.
  template <class F>
  void CompileDisjunction(int alt_num, F&& gen_alt) {
    // An alternative a1 | ... | an is compiled into
    //
    //     FORK tail1
    //     <a1>
    //     JMP end
    //   tail1:
    //     FORK tail2
    //     <a2>
    //     JMP end
    //   tail2:
    //     ...
    //     ...
    //   tail{n -1}:
    //     <an>
    //   end:
    //
    // By the semantics of the FORK instruction (see above at definition and
    // semantics), a forked thread has lower priority than the thread that
    // spawned it.  This means that with the code we're generating here, the
    // thread matching the alternative a1 has indeed highest priority, followed
    // by the thread for a2 and so on.

    if (alt_num == 0) {
      // The empty disjunction.  This can never match.
      assembler_.Fail();
      return;
    }

    Label end;

    for (int i = 0; i != alt_num - 1; ++i) {
      Label tail;
      assembler_.Fork(tail);
      gen_alt(i);
      assembler_.Jmp(end);
      assembler_.Bind(tail);
    }

    gen_alt(alt_num - 1);

    assembler_.Bind(end);
  }

  void* VisitDisjunction(RegExpDisjunction* node, void*) override {
    ZoneList<RegExpTree*>& alts = *node->alternatives();
    CompileDisjunction(alts.length(),
                       [&](int i) { alts[i]->Accept(this, nullptr); });
    return nullptr;
  }

  void* VisitAlternative(RegExpAlternative* node, void*) override {
    for (RegExpTree* child : *node->nodes()) {
      child->Accept(this, nullptr);
    }
    return nullptr;
  }

  void* VisitAssertion(RegExpAssertion* node, void*) override {
    assembler_.Assertion(node->assertion_type());
    return nullptr;
  }

  void* VisitCharacterClass(RegExpCharacterClass* node, void*) override {
    // A character class is compiled as Disjunction over its `CharacterRange`s.
    ZoneList<CharacterRange>* ranges = node->ranges(zone_);
    CharacterRange::Canonicalize(ranges);
    if (node->is_negated()) {
      // The complement of a disjoint, non-adjacent (i.e. `Canonicalize`d)
      // union of k intervals is a union of at most k + 1 intervals.
      ZoneList<CharacterRange>* negated =
          zone_->New<ZoneList<CharacterRange>>(ranges->length() + 1, zone_);
      CharacterRange::Negate(ranges, negated, zone_);
      DCHECK_LE(negated->length(), ranges->length() + 1);
      ranges = negated;
    }

    CompileDisjunction(ranges->length(), [&](int i) {
      // We don't support utf16 for now, so only ranges that can be specified
      // by (complements of) ranges with base::uc16 bounds.
      STATIC_ASSERT(kMaxSupportedCodepoint <=
                    std::numeric_limits<base::uc16>::max());

      base::uc32 from = (*ranges)[i].from();
      DCHECK_LE(from, kMaxSupportedCodepoint);
      base::uc16 from_uc16 = static_cast<base::uc16>(from);

      base::uc32 to = (*ranges)[i].to();
      DCHECK_IMPLIES(to > kMaxSupportedCodepoint, to == String::kMaxCodePoint);
      base::uc16 to_uc16 =
          static_cast<base::uc16>(std::min(to, kMaxSupportedCodepoint));

      assembler_.ConsumeRange(from_uc16, to_uc16);
    });
    return nullptr;
  }

  void* VisitAtom(RegExpAtom* node, void*) override {
    for (base::uc16 c : node->data()) {
      assembler_.ConsumeRange(c, c);
    }
    return nullptr;
  }

  void ClearRegisters(Interval indices) {
    if (indices.is_empty()) return;
    DCHECK_EQ(indices.from() % 2, 0);
    DCHECK_EQ(indices.to() % 2, 1);
    for (int i = indices.from(); i <= indices.to(); i += 2) {
      // It suffices to clear the register containing the `begin` of a capture
      // because this indicates that the capture is undefined, regardless of
      // the value in the `end` register.
      assembler_.ClearRegister(i);
    }
  }

  // Emit bytecode corresponding to /<emit_body>*/.
  template <class F>
  void CompileGreedyStar(F&& emit_body) {
    // This is compiled into
    //
    //   begin:
    //     FORK end
    //     <body>
    //     JMP begin
    //   end:
    //     ...
    //
    // This is greedy because a forked thread has lower priority than the
    // thread that spawned it.
    Label begin;
    Label end;

    assembler_.Bind(begin);
    assembler_.Fork(end);
    emit_body();
    assembler_.Jmp(begin);

    assembler_.Bind(end);
  }

  // Emit bytecode corresponding to /<emit_body>*?/.
  template <class F>
  void CompileNonGreedyStar(F&& emit_body) {
    // This is compiled into
    //
    //     FORK body
    //     JMP end
    //   body:
    //     <body>
    //     FORK body
    //   end:
    //     ...

    Label body;
    Label end;

    assembler_.Fork(body);
    assembler_.Jmp(end);

    assembler_.Bind(body);
    emit_body();
    assembler_.Fork(body);

    assembler_.Bind(end);
  }

  // Emit bytecode corresponding to /<emit_body>{0, max_repetition_num}/.
  template <class F>
  void CompileGreedyRepetition(F&& emit_body, int max_repetition_num) {
    // This is compiled into
    //
    //     FORK end
    //     <body>
    //     FORK end
    //     <body>
    //     ...
    //     ...
    //     FORK end
    //     <body>
    //   end:
    //     ...

    Label end;
    for (int i = 0; i != max_repetition_num; ++i) {
      assembler_.Fork(end);
      emit_body();
    }
    assembler_.Bind(end);
  }

  // Emit bytecode corresponding to /<emit_body>{0, max_repetition_num}?/.
  template <class F>
  void CompileNonGreedyRepetition(F&& emit_body, int max_repetition_num) {
    // This is compiled into
    //
    //     FORK body0
    //     JMP end
    //   body0:
    //     <body>
    //     FORK body1
    //     JMP end
    //   body1:
    //     <body>
    //     ...
    //     ...
    //   body{max_repetition_num - 1}:
    //     <body>
    //   end:
    //     ...

    Label end;
    for (int i = 0; i != max_repetition_num; ++i) {
      Label body;
      assembler_.Fork(body);
      assembler_.Jmp(end);

      assembler_.Bind(body);
      emit_body();
    }
    assembler_.Bind(end);
  }

  void* VisitQuantifier(RegExpQuantifier* node, void*) override {
    // Emit the body, but clear registers occuring in body first.
    //
    // TODO(mbid,v8:10765): It's not always necessary to a) capture registers
    // and b) clear them. For example, we don't have to capture anything for
    // the first 4 repetitions if node->min() >= 5, and then we don't have to
    // clear registers in the first node->min() repetitions.
    // Later, and if node->min() == 0, we don't have to clear registers before
    // the first optional repetition.
    Interval body_registers = node->body()->CaptureRegisters();
    auto emit_body = [&]() {
      ClearRegisters(body_registers);
      node->body()->Accept(this, nullptr);
    };

    // First repeat the body `min()` times.
    for (int i = 0; i != node->min(); ++i) emit_body();

    switch (node->quantifier_type()) {
      case RegExpQuantifier::POSSESSIVE:
        UNREACHABLE();
      case RegExpQuantifier::GREEDY: {
        if (node->max() == RegExpTree::kInfinity) {
          CompileGreedyStar(emit_body);
        } else {
          DCHECK_NE(node->max(), RegExpTree::kInfinity);
          CompileGreedyRepetition(emit_body, node->max() - node->min());
        }
        break;
      }
      case RegExpQuantifier::NON_GREEDY: {
        if (node->max() == RegExpTree::kInfinity) {
          CompileNonGreedyStar(emit_body);
        } else {
          DCHECK_NE(node->max(), RegExpTree::kInfinity);
          CompileNonGreedyRepetition(emit_body, node->max() - node->min());
        }
      }
    }
    return nullptr;
  }

  void* VisitCapture(RegExpCapture* node, void*) override {
    int index = node->index();
    int start_register = RegExpCapture::StartRegister(index);
    int end_register = RegExpCapture::EndRegister(index);
    assembler_.SetRegisterToCp(start_register);
    node->body()->Accept(this, nullptr);
    assembler_.SetRegisterToCp(end_register);
    return nullptr;
  }

  void* VisitGroup(RegExpGroup* node, void*) override {
    node->body()->Accept(this, nullptr);
    return nullptr;
  }

  void* VisitLookaround(RegExpLookaround* node, void*) override {
    // TODO(mbid,v8:10765): Support this case.
    UNREACHABLE();
  }

  void* VisitBackReference(RegExpBackReference* node, void*) override {
    UNREACHABLE();
  }

  void* VisitEmpty(RegExpEmpty* node, void*) override { return nullptr; }

  void* VisitText(RegExpText* node, void*) override {
    for (TextElement& text_el : *node->elements()) {
      text_el.tree()->Accept(this, nullptr);
    }
    return nullptr;
  }

 private:
  Zone* zone_;
  BytecodeAssembler assembler_;
};

}  // namespace

ZoneList<RegExpInstruction> ExperimentalRegExpCompiler::Compile(
    RegExpTree* tree, JSRegExp::Flags flags, Zone* zone) {
  return CompileVisitor::Compile(tree, flags, zone);
}

}  // namespace internal
}  // namespace v8