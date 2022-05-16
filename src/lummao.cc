#include <cmath>
#include <fstream>

#include <tailslide/tailslide.hh>
#include <tailslide/passes/desugaring.hh>

using namespace Tailslide;

class PythonVisitor : public ASTVisitor {
  protected:
  void writeChildrenSep(LSLASTNode *parent, const char *separator);
  void writeFloat(float f_val);

  virtual bool visit(LSLScript *script);
  virtual bool visit(LSLGlobalVariable *glob_var);
  virtual bool visit(LSLGlobalFunction *glob_func);
  virtual bool visit(LSLEventHandler *event_handler);

  virtual bool visit(LSLIntegerConstant *int_const);
  virtual bool visit(LSLFloatConstant *float_const);
  virtual bool visit(LSLStringConstant *str_const);
  virtual bool visit(LSLKeyConstant *key_const);
  virtual bool visit(LSLVectorConstant *vec_const);
  virtual bool visit(LSLQuaternionConstant *quat_const);
  virtual bool visit(LSLVectorExpression *vec_expr);
  virtual bool visit(LSLQuaternionExpression *quat_expr);
  virtual bool visit(LSLListConstant *list_const);
  virtual bool visit(LSLListExpression *list_expr);
  virtual bool visit(LSLTypecastExpression *cast_expr);
  virtual bool visit(LSLFunctionExpression *func_expr);
  virtual bool visit(LSLLValueExpression *lvalue);
  void constructMutatedMember(LSLSymbol *sym, LSLIdentifier *member, LSLExpression *rhs);
  virtual bool visit(LSLBinaryExpression *bin_expr);
  virtual bool visit(LSLUnaryExpression *unary_expr);
  virtual bool visit(LSLPrintExpression *print_expr);
  virtual bool visit(LSLParenthesisExpression *parens_expr);
  virtual bool visit(LSLBoolConversionExpression *bool_expr);
  virtual bool visit(LSLConstantExpression *const_expr) { return true; }

  virtual bool visit(LSLNopStatement *nop_stmt);
  virtual bool visit(LSLCompoundStatement *compound_stmt);
  virtual bool visit(LSLExpressionStatement *expr_stmt);
  virtual bool visit(LSLDeclaration *decl_stmt);
  virtual bool visit(LSLIfStatement *if_stmt);
  virtual bool visit(LSLForStatement *for_stmt);
  virtual bool visit(LSLWhileStatement *while_stmt);
  virtual bool visit(LSLDoStatement *do_stmt);
  virtual bool visit(LSLJumpStatement *jump_stmt);
  virtual bool visit(LSLLabel *label_stmt);
  virtual bool visit(LSLReturnStatement *return_stmt);
  virtual bool visit(LSLStateStatement *state_stmt);

  public:
  std::stringstream mStr;
  int mTabs = 0;

  void doTabs() {
    for(int i=0; i<mTabs; ++i) {
      mStr << "    ";
    }
  }
};

static const char * const PY_TYPE_NAMES[LST_MAX] {
  "None",
  "int",
  "float",
  "str",
  "Key",
  "Vector",
  "Quaternion",
  "list",
  "<ERROR>"
};

class ScopedTabSetter {
  public:
    ScopedTabSetter(PythonVisitor *visitor, int tabs): _mOldTabs(visitor->mTabs), _mVisitor(visitor) {
      _mVisitor->mTabs = tabs;
    };
    ~ScopedTabSetter() {
      _mVisitor->mTabs = _mOldTabs;
    }
  private:
    int _mOldTabs;
    PythonVisitor *_mVisitor;
};



void PythonVisitor::writeChildrenSep(LSLASTNode *parent, const char *separator) {
  for (auto *child: *parent) {
    child->visit(this);
    if (child->getNext())
      mStr << separator;
  }
}

void PythonVisitor::writeFloat(float f_val) {
  if (std::nearbyint((double)f_val) == (double)f_val) {
    // if it's int-like then we can write it in decimal form without loss of precision
    // but we need a special case for -0!
    if (f_val == 0 && std::signbit(f_val)) {
        mStr << "-0.0";
        return;
    }
    mStr << (int64_t)f_val << ".0";
    return;
  }
  // Write in host-endian binary form to preserve precision
  auto *b_val = reinterpret_cast<uint8_t *>(&f_val);
  const size_t s_val_len = (4 * 2) + 1;
  char s_val[s_val_len] = {0};
  snprintf(
      (char *)&s_val,
      s_val_len,
      "%02x%02x%02x%02x",
      b_val[0], b_val[1], b_val[2], b_val[3]
  );
  // the human-readable float val is first in the tuple, but it isn't actually used, it's only there
  // for readability.
  // TODO: some kind of heuristic for floats that are representable in float literal form.
  mStr << "bin2float('" << std::to_string(f_val) << "', '"<< (const char*)&s_val << "')";
}

bool PythonVisitor::visit(LSLScript *script) {
  // Need to make any casts explicit
  class DeSugaringVisitor de_sugaring_visitor(script->mContext->allocator, true);
  script->visit(&de_sugaring_visitor);
  mStr << "from lummao import *\n\n\n";
  mStr << "class Script(BaseLSLScript):\n";
  // everything after this must be indented
  ScopedTabSetter tab_setter(this, mTabs + 1);

  // put the type declarations for global vars at the class level
  for (auto *glob : *script->getGlobals()) {
    if (glob->getNodeType() != NODE_GLOBAL_VARIABLE)
      continue;
    auto *glob_var = (LSLGlobalVariable *)glob;
    auto *id = glob_var->getIdentifier();
    doTabs();
    mStr << id->getName() << ": " << PY_TYPE_NAMES[id->getIType()] << '\n';
  }

  mStr << '\n';
  // then generate an __init__() where they're actually initialized
  doTabs();
  mStr << "def __init__(self):\n";
  {
    // needs to be indented one more level within the __init__()
    ScopedTabSetter tab_setter_2(this, mTabs + 1);
    doTabs();
    mStr << "super().__init__()\n";
    for (auto *glob: *script->getGlobals()) {
      if (glob->getNodeType() != NODE_GLOBAL_VARIABLE)
        continue;
      glob->visit(this);
    }
    mStr << '\n';
  }

  // now the global functions
  for (auto *glob : *script->getGlobals()) {
    if (glob->getNodeType() != NODE_GLOBAL_FUNCTION)
      continue;
    glob->visit(this);
  }

  // and the states and their event handlers
  script->getStates()->visit(this);
  return false;
}

bool PythonVisitor::visit(LSLGlobalVariable *glob_var) {
  auto *sym = glob_var->getSymbol();
  doTabs();
  mStr << "self." << sym->getName() << " = ";
  LSLASTNode *initializer = glob_var->getInitializer();
  if (!initializer) {
    initializer = sym->getType()->getDefaultValue();
  }
  initializer->visit(this);
  mStr << "\n";
  return false;
}

bool PythonVisitor::visit(LSLGlobalFunction *glob_func) {
  auto *id = glob_func->getIdentifier();
  doTabs();
  mStr << "@with_goto\n";
  doTabs();
  mStr << "def " << id->getName() << "(self";
  for (auto *arg : *glob_func->getArguments()) {
    mStr << ", " << arg->getName() << ": " << PY_TYPE_NAMES[arg->getIType()];
  }
  mStr << ") -> " << PY_TYPE_NAMES[id->getIType()] << ":\n";
  ScopedTabSetter tab_setter(this, mTabs + 1);
  glob_func->getStatements()->visit(this);
  mStr << '\n';
  return false;
}

bool PythonVisitor::visit(LSLEventHandler *event_handler) {
  auto *id = event_handler->getIdentifier();
  auto *state_sym = event_handler->getParent()->getParent()->getSymbol();
  doTabs();
  mStr << "@with_goto\n";
  doTabs();
  mStr << "def e" << state_sym->getName() << id->getName() << "(self";
  for (auto *arg : *event_handler->getArguments()) {
    mStr << ", " << arg->getName() << ": " << PY_TYPE_NAMES[arg->getIType()];
  }
  mStr << ") -> " << PY_TYPE_NAMES[id->getIType()] << ":\n";
  ScopedTabSetter tab_setter(this, mTabs + 1);
  event_handler->getStatements()->visit(this);
  mStr << '\n';
  return false;
}

bool PythonVisitor::visit(LSLIntegerConstant *int_const) {
  // Usually you'd need an `S32()`, but we natively deal in int32 anyway.
  mStr << int_const->getValue();
  return false;
}

bool PythonVisitor::visit(LSLFloatConstant *float_const) {
  writeFloat(float_const->getValue());
  return false;
}

bool PythonVisitor::visit(LSLStringConstant *str_const) {
  // TODO: Probably not correctly accounting for encoding.
  mStr << '"' << escape_string(str_const->getValue()) << '"';
  return false;
}

bool PythonVisitor::visit(LSLKeyConstant *key_const) {
  // TODO: Probably not correctly accounting for encoding.
  mStr << "Key(\"" << escape_string(key_const->getValue()) << "\")";
  return false;
}

bool PythonVisitor::visit(LSLVectorConstant *vec_const) {
  auto *val = vec_const->getValue();
  mStr << "Vector((";
  writeFloat(val->x);
  mStr << ", ";
  writeFloat(val->y);
  mStr << ", ";
  writeFloat(val->z);
  mStr << "))";
  return false;
}

bool PythonVisitor::visit(LSLQuaternionConstant *quat_const) {
  auto *val = quat_const->getValue();
  mStr << "Quaternion((";
  writeFloat(val->x);
  mStr << ", ";
  writeFloat(val->y);
  mStr << ", ";
  writeFloat(val->z);
  mStr << ", ";
  writeFloat(val->s);
  mStr << "))";
  return false;
}

bool PythonVisitor::visit(LSLVectorExpression *vec_expr) {
  mStr << "Vector((";
  writeChildrenSep(vec_expr, ", ");
  mStr << "))";
  return false;
}

bool PythonVisitor::visit(LSLQuaternionExpression *quat_expr) {
  mStr << "Quaternion((";
  writeChildrenSep(quat_expr, ", ");
  mStr << "))";
  return false;
}

bool PythonVisitor::visit(LSLTypecastExpression *cast_expr) {
  auto *child_expr = cast_expr->getChildExpr();
  auto from_type = child_expr->getIType();
  auto to_type = cast_expr->getIType();
  if (from_type == LST_INTEGER && to_type == LST_FLOATINGPOINT) {
    // this is less annoying to read and basically the same thing
    mStr << "float(";
    child_expr->visit(this);
    mStr << ')';
    return false;
  }
  mStr << "typecast(";
  child_expr->visit(this);
  mStr << ", " << PY_TYPE_NAMES[cast_expr->getIType()] << ")";
  return false;
}

bool PythonVisitor::visit(LSLListConstant *list_const) {
  mStr << '[';
  writeChildrenSep(list_const, ", ");
  mStr << ']';
  return false;
}

bool PythonVisitor::visit(LSLListExpression *list_expr) {
  mStr << '[';
  writeChildrenSep(list_expr, ", ");
  mStr << ']';
  return false;
}

bool PythonVisitor::visit(LSLFunctionExpression *func_expr) {
  auto *sym = func_expr->getSymbol();
  if (sym->getSubType() == SYM_BUILTIN) {
    mStr << "lslfuncs.";
  } else {
    mStr << "self.";
  }
  mStr << sym->getName() << "(";
  for (auto *arg : *func_expr->getArguments()) {
    arg->visit(this);
    if (arg->getNext())
      mStr << ", ";
  }
  mStr << ')';
  return false;
}

static int member_to_offset(const char *member) {
  // Vector and Quaternion aren't namedtuples so we can't do the nice thing.
  int offset;
  switch(member[0]) {
    case 'x': offset = 0; break;
    case 'y': offset = 1; break;
    case 'z': offset = 2; break;
    case 's': offset = 3; break;
    default:
      offset = 0;
      assert(0);
  }
  return offset;
}

bool PythonVisitor::visit(LSLLValueExpression *lvalue) {
  if (lvalue->getSymbol()->getSubType() == SYM_GLOBAL)
    mStr << "self.";
  mStr << lvalue->getIdentifier()->getName();
  if (auto *member = lvalue->getMember()) {
    mStr << '[' << member_to_offset(member->getName()) << ']';
  }
  return false;
}

void PythonVisitor::constructMutatedMember(LSLSymbol *sym, LSLIdentifier *member, LSLExpression *rhs) {
  // Member case is special. We actually need to construct a new version of the same
  // type of object with only the selected member swapped out, and then assign _that_.
  int member_offset = member_to_offset(member->getName());
  mStr << "replace_coord_axis(";
  if (sym->getSubType() == SYM_GLOBAL)
    mStr << "self.";
  mStr << sym->getName() << ", " << member_offset << ", ";
  rhs->visit(this);
  mStr << ')';
}

bool PythonVisitor::visit(LSLBinaryExpression *bin_expr) {
  auto op = bin_expr->getOperation();
  auto *lhs = bin_expr->getLHS();
  auto *rhs = bin_expr->getRHS();

  if (op == '=') {
    auto *lvalue = (LSLLValueExpression *) lhs;
    auto *sym = lvalue->getSymbol();
    // If our result isn't needed, this expression will be put in a statement context in Python.
    // We can just directly assign, no special song and dance. There are some other cases where
    // we can do this but we'll worry about them later since they don't come up as often.
    if (!bin_expr->getResultNeeded()) {
      if (sym->getSubType() == SYM_GLOBAL)
        mStr << "self.";
      mStr << sym->getName() << " = ";
      if (auto *member = lvalue->getMember()) {
        constructMutatedMember(sym, member, rhs);
      } else {
        rhs->visit(this);
      }
    } else {
      if (sym->getSubType() == SYM_GLOBAL) {
        // walrus operator can't assign to these, need to use special assignment helper.
        mStr << "assign(self.__dict__, \"" << sym->getName() << "\", ";
      } else {
        // We need to wrap this assignment in parens so we can use the walrus operator.
        // walrus operator works regardless of expression or statement context, but doesn't
        // work for cases like `(self.foo := 2)` where we're assigning to an attribute rather than
        // just a single identifier...
        mStr << '(' << sym->getName() << " := ";
      }

      if (auto *member = lvalue->getMember()) {
        constructMutatedMember(sym, member, rhs);
      } else {
        rhs->visit(this);
      }
      mStr << ')';
      if (auto *member = lvalue->getMember()) {
        mStr << '[' << member_to_offset(member->getName()) << ']';
      }
    }
    return false;
  }
  if (op == OP_MUL_ASSIGN) {
    // int *= float case
    auto *sym = lhs->getSymbol();
    if (sym->getSubType() == SYM_GLOBAL) {
      // walrus operator can't assign to these, need to use special assignment helper.
      mStr << "assign(self.__dict__, \"" << sym->getName() << "\", ";
    } else {
      mStr << '(' << sym->getName() << " := ";
    }
    // don't have to consider the member case, no such thing as coordinates with int members.
    mStr << "typecast(rmul(";
    rhs->visit(this);
    mStr << ", ";
    lhs->visit(this);
    mStr << "), int))";
    return false;
  }
  switch(op) {
    case '+':            mStr << "radd("; break;
    case '-':            mStr << "rsub("; break;
    case '*':            mStr << "rmul("; break;
    case '/':            mStr << "rdiv("; break;
    case '%':            mStr << "rmod("; break;
    case OP_EQ:          mStr << "req("; break;
    case OP_NEQ:         mStr << "rneq("; break;
    case OP_GREATER:     mStr << "rgreater("; break;
    case OP_LESS:        mStr << "rless("; break;
    case OP_GEQ:         mStr << "rgeq("; break;
    case OP_LEQ:         mStr << "rleq("; break;
    case OP_BOOLEAN_AND: mStr << "rbooland("; break;
    case OP_BOOLEAN_OR:  mStr << "rboolor("; break;
    case OP_BIT_AND:     mStr << "rbitand("; break;
    case OP_BIT_OR:      mStr << "rbitor("; break;
    case OP_BIT_XOR:     mStr << "rbitxor("; break;
    case OP_SHIFT_LEFT:  mStr << "rshl("; break;
    case OP_SHIFT_RIGHT: mStr << "rshr("; break;
    default:
      assert(0);
      mStr << "<ERROR>";
  }
  rhs->visit(this);
  mStr << ", ";
  lhs->visit(this);
  mStr << ')';
  return false;
}

bool PythonVisitor::visit(LSLUnaryExpression *unary_expr) {
  auto *child_expr = unary_expr->getChildExpr();
  auto op = unary_expr->getOperation();
  if (op == OP_POST_DECR || op == OP_POST_INCR || op == OP_PRE_DECR || op == OP_PRE_INCR) {
    int post = op == OP_POST_INCR || op == OP_POST_DECR;
    int negative = op == OP_POST_DECR || op == OP_PRE_DECR;
    auto *lvalue = (LSLLValueExpression *) child_expr;
    auto *sym = lvalue->getSymbol();
    bool global = sym->getSubType() == SYM_GLOBAL;
    auto *member = lvalue->getMember();

    if (unary_expr->getResultNeeded() || member) {
      // this is in expression context, not statement context. We need to emulate the
      // side-effects of ++foo and foo++ in an expression, since that construct doesn't exist
      // in python.
      if (post)
        mStr << "post";
      else
        mStr << "pre";

      if (negative)
        mStr << "decr";
      else
        mStr << "incr";

      mStr << '(';
      if (global)
        mStr << "self.__dict__";
      else
        mStr << "locals()";
      mStr << ", \"" << sym->getName() << "\"";
      if (auto *member = lvalue->getMember()) {
        mStr << ", " << member_to_offset(member->getName());
      }
      mStr << ')';
    } else {
      // in statement context, we can use the more idiomatic foo += 1 or foo -= 1.
      if (sym->getSubType() == SYM_GLOBAL)
        mStr << "self.";
      mStr << sym->getName();
      if (op == OP_POST_DECR || op == OP_PRE_DECR) {
        mStr << " -= ";
      } else {
        mStr << " += ";
      }
      child_expr->getType()->getOneValue()->visit(this);
    }
    return false;
  }

  switch (op) {
    case '-': mStr << "neg("; break;
    case '~': mStr << "bitnot("; break;
    case '!': mStr << "boolnot("; break;
    default:
      assert(0);
      mStr << "<ERROR>";
  }
  child_expr->visit(this);
  mStr << ')';
  return false;
}

bool PythonVisitor::visit(LSLPrintExpression *print_expr) {
  mStr << "print(";
  print_expr->getChildExpr()->visit(this);
  mStr << ')';
  return false;
}

bool PythonVisitor::visit(LSLParenthesisExpression *parens_expr) {
  mStr << "(";
  parens_expr->getChildExpr()->visit(this);
  mStr << ')';
  return false;
}

bool PythonVisitor::visit(LSLBoolConversionExpression *bool_expr) {
  mStr << "cond(";
  bool_expr->getChildExpr()->visit(this);
  mStr << ")";
  return false;
}


bool PythonVisitor::visit(LSLNopStatement *nop_stmt) {
  doTabs();
  mStr << "pass\n";
  return false;
}

bool PythonVisitor::visit(LSLCompoundStatement *compound_stmt) {
  if (compound_stmt->hasChildren()) {
    visitChildren(compound_stmt);
  } else {
    doTabs();
    mStr << "pass\n";
  }
  return false;
}

bool PythonVisitor::visit(LSLExpressionStatement *expr_stmt) {
  doTabs();
  expr_stmt->getExpr()->visit(this);
  mStr << '\n';
  return false;
}

bool PythonVisitor::visit(LSLDeclaration *decl_stmt) {
  doTabs();
  auto *sym = decl_stmt->getSymbol();
  mStr << sym->getName() << ": " << PY_TYPE_NAMES[sym->getIType()] << " = ";
  LSLASTNode *initializer = decl_stmt->getInitializer();
  if (!initializer) {
    initializer = sym->getType()->getDefaultValue();
  }
  initializer->visit(this);
  mStr << "\n";
  return false;
}

bool PythonVisitor::visit(LSLIfStatement *if_stmt) {
  doTabs();
  mStr << "if ";
  if_stmt->getCheckExpr()->visit(this);
  mStr << ":\n";
  {
    ScopedTabSetter tab_setter(this, mTabs + 1);
    if_stmt->getTrueBranch()->visit(this);
  }
  if(auto *false_branch = if_stmt->getFalseBranch()) {
    doTabs();
    mStr << "else:\n";
    {
      ScopedTabSetter tab_setter(this, mTabs + 1);
      false_branch->visit(this);
    }
  }
  return false;
}

bool PythonVisitor::visit(LSLForStatement *for_stmt) {
  // initializer expressions come as ExpressionStatements before the actual loop
  for (auto *init_expr : *for_stmt->getInitExprs()) {
    doTabs();
    init_expr->visit(this);
    mStr << '\n';
  }
  // all loops are represented as `while`s in Python for consistency
  // since LSL's loop semantics are different from Python's
  doTabs();
  mStr << "while True:\n";
  {
    ScopedTabSetter tab_setter_1(this, mTabs + 1);

    doTabs();
    mStr << "if not ";
    for_stmt->getCheckExpr()->visit(this);
    mStr << ":\n";
    {
      ScopedTabSetter tab_setter_2(this, mTabs + 1);
      doTabs();
      mStr << "break\n";
    }

    for_stmt->getBody()->visit(this);
    for (auto *incr_expr : *for_stmt->getIncrExprs()) {
      doTabs();
      incr_expr->visit(this);
      mStr << '\n';
    }
  }
  return false;
}

bool PythonVisitor::visit(LSLWhileStatement *while_stmt) {
  doTabs();
  mStr << "while ";
  while_stmt->getCheckExpr()->visit(this);
  mStr << ":\n";
  {
    ScopedTabSetter tab_setter_1(this, mTabs + 1);
    while_stmt->getBody()->visit(this);
  }
  return false;
}

bool PythonVisitor::visit(LSLDoStatement *do_stmt) {
  doTabs();
  mStr << "while True:\n";
  {
    ScopedTabSetter tab_setter_1(this, mTabs + 1);
    do_stmt->getBody()->visit(this);
    doTabs();
    mStr << "if not ";
    do_stmt->getCheckExpr()->visit(this);
    mStr << ":\n";
    {
      ScopedTabSetter tab_setter_2(this, mTabs + 1);
      doTabs();
      mStr << "break\n";
    }
  }
  return false;
}

bool PythonVisitor::visit(LSLJumpStatement *jump_stmt) {
  doTabs();
  // We could check `continueLike` or `breakLike` here, but
  // LSL's `for` semantics differ from Python's, so we'd have to use
  // an exception in the `for` case anyways. No sense in pretending
  // we have structured jumps when we really don't, I guess.
  mStr << "goto ." << jump_stmt->getSymbol()->getName() << "\n";
  return false;
}

bool PythonVisitor::visit(LSLLabel *label_stmt) {
  doTabs();
  mStr << "label ." << label_stmt->getSymbol()->getName() << "\n";
  return false;
}

bool PythonVisitor::visit(LSLReturnStatement *return_stmt) {
  doTabs();
  if (auto *expr = return_stmt->getExpr()) {
    mStr << "return ";
    expr->visit(this);
  } else {
    mStr << "return";
  }
  mStr << '\n';
  return false;
}

bool PythonVisitor::visit(LSLStateStatement *state_stmt) {
  doTabs();
  mStr << "raise StateChangeException('" << state_stmt->getSymbol()->getName() << "')\n";
  return false;
}



int main(int argc, char **argv) {
  FILE *yyin = nullptr;

  if (argc != 3) {
    fprintf(stderr, "lummao <lsl_script> <out_py>\n");
    return 1;
  }

  if (strcmp(argv[1], "-") != 0) {
    yyin = fopen(argv[1], "r");
    if (yyin == nullptr) {
      fprintf(stderr, "couldn't open %s\n", argv[1]);
      return 1;
    }
  }

  tailslide_init_builtins(nullptr);
  // set up the allocator and logger
  ScopedScriptParser parser(nullptr);
  Logger *logger = &parser.logger;

  auto script = parser.parseLSL(yyin);
  if (yyin != nullptr)
    fclose(yyin);

  if (script) {
    script->collectSymbols();
    script->determineTypes();
    script->recalculateReferenceData();
    script->propagateValues();
    script->checkBestPractices();

    if (!logger->getErrors()) {
      script->validateGlobals(true);
      script->checkSymbols();
    }
  }
  logger->report();

  if (!logger->getErrors()) {
    PythonVisitor py_visitor;
    script->visit(&py_visitor);
    std::string py_code {py_visitor.mStr.str()};
    if (!strcmp(argv[2], "-")) {
      fprintf(stdout, "%s", py_code.c_str());
    } else {
      std::ofstream py_out(argv[2], std::ios_base::binary | std::ios_base::out);
      if (py_out.good()) {
        py_out.write(py_code.c_str(), (int) py_code.size());
      } else {
        fprintf(stderr, "Couldn't open '%s'\n", argv[2]);
        return 1;
      }
    }
  }
  return logger->getErrors();
}
