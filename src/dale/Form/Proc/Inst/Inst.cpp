#include "Inst.h"

#include "../../../Units/Units.h"
#include "../../../Node/Node.h"
#include "../../../ParseResult/ParseResult.h"
#include "../../../Function/Function.h"
#include "../../../llvm_Function.h"
#include "../../../Operation/Copy/Copy.h"
#include "../../../CoreForms/CoreForms.h"

#include "../Token/Token.h"
#include "../Funcall/Funcall.h"
#include "../../Function/Function.h"
#include "../../Literal/Struct/Struct.h"
#include "../../Literal/Enum/Enum.h"
#include "../../Literal/Array/Array.h"
#include "../../Type/Type.h"

#define eq(str_arg) !strcmp(str, str_arg)

using namespace dale::ErrorInst::Generator;

namespace dale
{
static int anoncount = 0;

bool
createAnonymousFunction(Units *units, llvm::BasicBlock *block,
                        Node *n, ParseResult *pr)
{
    Context *ctx = units->top()->ctx;
    int preindex = ctx->lv_index;

    std::vector<NSNode *> active_ns_nodes = ctx->active_ns_nodes;
    std::vector<NSNode *> used_ns_nodes   = ctx->used_ns_nodes;
    ctx->popUntilNamespace(units->prefunction_ns);

    int error_count_begin = ctx->er->getErrorTypeCount(ErrorType::Error);

    char buf[16];
    sprintf(buf, "_anon_%d", anoncount++);
    Function *anon_fn = NULL;
    FormFunctionParse(units, n, buf, &anon_fn, Linkage::Intern, 1);

    int error_count_end = ctx->er->getErrorTypeCount(ErrorType::Error);

    if (error_count_begin != error_count_end) {
        ctx->active_ns_nodes = active_ns_nodes;
        ctx->used_ns_nodes   = used_ns_nodes;
        return false;
    }

    Type *fn_type = new Type();
    fn_type->is_function = 1;
    fn_type->return_type = anon_fn->return_type;

    for (std::vector<Variable *>::iterator
            b = anon_fn->parameter_types.begin(),
            e = anon_fn->parameter_types.end();
            b != e;
            ++b) {
        fn_type->parameter_types.push_back((*b)->type);
    }

    pr->set(block, ctx->tr->getPointerType(fn_type),
            llvm::cast<llvm::Value>(anon_fn->llvm_function));

    std::vector<Variable *> vars;
    ctx->ns()->getVarsAfterIndex(preindex, &vars);
    for (std::vector<Variable *>::iterator b = vars.begin(),
                                           e = vars.end();
            b != e;
            ++b) {
        (*b)->index = 0;
    }

    ctx->active_ns_nodes = active_ns_nodes;
    ctx->used_ns_nodes   = used_ns_nodes;

    return true;
}

bool
createWantedStructLiteral(Units *units, Function *fn, llvm::BasicBlock *block,
                          Node *n, bool get_address,
                          Type *wanted_type, ParseResult *pr)
{
    Context *ctx = units->top()->ctx;
    Struct *str =
        ctx->getStruct(wanted_type->struct_name.c_str(),
                       &(wanted_type->namespaces));
    assert(str && "cannot load struct");

    bool res =
        FormLiteralStructParse(units, fn, block, n,
                               wanted_type->struct_name.c_str(),
                               str, wanted_type, get_address, pr);
    return res;
}

bool
createEnumLiteral(Units *units, Function *fn, llvm::BasicBlock *block,
                  Node *n, bool get_address, Type *wanted_type,
                  ParseResult *pr)
{
    Context *ctx = units->top()->ctx;
    std::vector<Node *> *lst = n->list;
    const char *name = lst->at(0)->token->str_value.c_str();

    Struct *st = ctx->getStruct(name);
    assert(st && "no struct associated with enum");

    Type *enum_type = FormTypeParse(units, (*lst)[0], false, false);
    assert(enum_type && "no type associated with enum");

    int original_error_count =
        ctx->er->getErrorTypeCount(ErrorType::Error);

    Enum *enum_obj = ctx->getEnum(name);
    bool res = FormLiteralEnumParse(units, block, (*lst)[1],
                                    enum_obj, enum_type,
                                    st, get_address, pr);
    if (!res) {
        ctx->er->popErrors(original_error_count);
    }
    return res;
}

bool
createStructLiteral(Units *units, Function *fn, llvm::BasicBlock *block,
                    Node *n, bool get_address, Type *wanted_type,
                    ParseResult *pr)
{
    Context *ctx = units->top()->ctx;
    std::vector<Node *> *lst = n->list;
    const char *name = lst->at(0)->token->str_value.c_str();

    Struct *st = ctx->getStruct(name);
    assert(st && "cannot load struct");

    Type *struct_type = FormTypeParse(units, (*lst)[0], false, false);
    assert(struct_type && "struct type does not exist");

    int original_error_count =
        ctx->er->getErrorTypeCount(ErrorType::Error);

    bool res = FormLiteralStructParse(units, fn, block, (*lst)[1],
                                      "flsp", st, struct_type,
                                      get_address, pr);
    if (!res) {
        ctx->er->popErrors(original_error_count);
    }
    return res;
}

bool
parsePotentialProcCall(Units *units, Function *fn, llvm::BasicBlock *block,
                       Node *n, bool get_address, Type *wanted_type,
                       ParseResult *pr, Error **backup_error)
{
    Context *ctx = units->top()->ctx;
    std::vector<Node *> *lst = n->list;
    Token *t = lst->at(0)->token;

    Function *fn_exists =
        ctx->getFunction(t->str_value.c_str(), NULL, NULL, 0);
    Function *mac_exists =
        ctx->getFunction(t->str_value.c_str(), NULL, NULL, 1);

    if (!fn_exists && !mac_exists) {
        return true;
    }

    int error_count_begin = ctx->er->getErrorTypeCount(ErrorType::Error);

    /* A function (or macro) with this name exists.  Call
     * parseFunctionCall: if it returns a PR, then great.  If it
     * returns no PR, but sets macro_to_call, then pass off to
     * parseMacroCall.  If it returns no PR, then pop the errors and
     * continue, but only if there is one error, and it's related to
     * an overloaded function not being present. */

    Function *macro_to_call = NULL;

    bool res =
        units->top()->fp->parseFunctionCall(fn, block, n,
                                            t->str_value.c_str(),
                                            get_address,
                                            &macro_to_call, pr);
    if (res) {
        return true;
    }

    if (macro_to_call) {
        Node *mac_node = units->top()->mp->parseMacroCall(n, macro_to_call);
        if (!mac_node) {
            return false;
        }
        bool res =
            FormProcInstParse(units, fn, block, mac_node,
                              get_address, false, wanted_type, pr);
        delete mac_node;
        return res;
    }

    int error_count_end = ctx->er->getErrorTypeCount(ErrorType::Error);

    if (error_count_end != (error_count_begin + 1)) {
        return false;
    }

    *backup_error = ctx->er->popLastError();
    if ((*backup_error)->getType() != ErrorType::Error) {
        ctx->er->addError(*backup_error);
        return false;
    }

    if (((*backup_error)->instance !=
            OverloadedFunctionOrMacroNotInScope)
            && ((*backup_error)->instance !=
                OverloadedFunctionOrMacroNotInScopeWithClosest)) {
        ctx->er->addError(*backup_error);
        return false;
    }

    return true;
}

bool
isFunctionObject(Context *ctx, ParseResult *pr)
{
    Type *inner_type = pr->type->points_to;

    if (inner_type && inner_type->struct_name.size()) {
        /* Struct must implement 'apply' to be considered a
         * function object. */
        Struct *st = ctx->getStruct(inner_type->struct_name.c_str(),
                                    &(inner_type->namespaces));
        if (st) {
            Type *apply = st->memberToType("apply");
            if (apply
                    && apply->points_to
                    && apply->points_to->is_function) {
                return true;
            }
        }
    }

    return false;
}

bool
parseFunctionObjectCall(Units *units, Function *fn, llvm::BasicBlock *block,
                        Node *n, bool get_address, bool prefixed_with_core,
                        Type *wanted_type, ParseResult *pr,
                        ParseResult *try_fp)
{
    Context *ctx = units->top()->ctx;
    std::vector<Node *> *lst = n->list;

    Type *try_fp_inner_type = try_fp->type->points_to;
    Struct *st = ctx->getStruct(try_fp_inner_type->struct_name.c_str(),
                                &(try_fp_inner_type->namespaces));
    Type *apply = st->memberToType("apply");

    /* The first argument of this function must be a pointer to this
     * particular struct type. */
    Type *apply_fn = apply->points_to;
    if (!(apply_fn->parameter_types.size())) {
        Error *e = new Error(ApplyMustTakePointerToStructAsFirstArgument,
                             (*lst)[0]);
        ctx->er->addError(e);
        return false;
    }
    if (!(apply_fn->parameter_types.at(0)->isEqualTo(try_fp->type))) {
        Error *e = new Error(ApplyMustTakePointerToStructAsFirstArgument,
                             (*lst)[0]);
        ctx->er->addError(e);
        return false;
    }

    /* Get the function pointer value. */
    std::vector<llvm::Value *> indices;
    STL::push_back2(&indices,
                    ctx->nt->getLLVMZero(),
                    ctx->nt->getNativeInt(
                        st->memberToIndex("apply")));

    llvm::IRBuilder<> builder(block);
    llvm::Value *res =
        builder.CreateGEP(try_fp->value,
                          llvm::ArrayRef<llvm::Value*>(indices));

    ParseResult apply_fn_pr;
    apply_fn_pr.set(block, apply,
                    llvm::cast<llvm::Value>(builder.CreateLoad(res)));

    /* So a pointer to the struct is your first argument.  Skip 1
     * element of the list when passing off (e.g. (adder 1)). */

    std::vector<llvm::Value*> extra_args;
    extra_args.push_back(try_fp->value);
    return
        units->top()->fp->parseFunctionPointerCall(fn, n, &apply_fn_pr,
                                                   1, &extra_args, pr);
}

bool
parseInternal(Units *units, Function *fn, llvm::BasicBlock *block,
              Node *n, bool get_address, bool prefixed_with_core,
              Type *wanted_type, ParseResult *pr)
{
    Context *ctx = units->top()->ctx;

    if (n->is_token) {
        return FormProcTokenParse(units, fn, block, n, get_address,
                                  prefixed_with_core, wanted_type, pr);
    }

    symlist *lst = n->list;
    if (lst->size() == 0) {
        Error *e = new Error(NoEmptyLists, n);
        ctx->er->addError(e);
        return false;
    }

    Node *first = (*lst)[0];
    if (!first->is_token) {
        (*lst)[0] = units->top()->mp->parsePotentialMacroCall(first);
        first = (*lst)[0];
        if (!first) {
            return false;
        }
    }

    /* If the first node is a token, and it equals "fn", then
     * create an anonymous function and return a pointer to it. */

    if (first->is_token and !first->token->str_value.compare("fn")) {
        return createAnonymousFunction(units, block, n, pr);
    }

    /* If wanted_type is present and is a struct, then use
     * FormLiteralStructParse, if the first list element is a list. */

    if (wanted_type
            && (wanted_type->struct_name.size())
            && (!first->is_token)) {
        return createWantedStructLiteral(units, fn, block, n, get_address,
                                         wanted_type, pr);
    }

    if (!first->is_token) {
        Error *e = new Error(FirstListElementMustBeAtom, n);
        ctx->er->addError(e);
        return false;
    }

    Token *t = first->token;
    if (t->type != TokenType::String) {
        Error *e = new Error(FirstListElementMustBeSymbol, n);
        ctx->er->addError(e);
        return false;
    }

    /* If the first element matches an enum name, then make an enum
     * literal (a struct literal) from the remainder of the form. */

    Enum *myenum = ctx->getEnum(t->str_value.c_str());
    if (myenum && (lst->size() == 2)) {
        bool res = createEnumLiteral(units, fn, block, n, get_address,
                                     wanted_type, pr);
        if (res) {
            return true;
        }
    }

    /* If the first element matches a struct name, then make a
     * struct literal from the remainder of the form. */

    Struct *st = ctx->getStruct(t->str_value.c_str());
    if (st && (lst->size() == 2)) {
        bool res = createStructLiteral(units, fn, block, n, get_address,
                                       wanted_type, pr);
        if (res) {
            return true;
        }
    }

    /* If the first element is 'array', and an array type has been
     * requested, handle that specially. */

    if (wanted_type
            && wanted_type->is_array
            && (!strcmp(t->str_value.c_str(), "array"))) {
        int size;
        bool res = FormLiteralArrayParse(units, fn, block, n,
                                         "array literal",
                                         wanted_type, get_address,
                                         &size, pr);
        return res;
    }

    /* Check that a macro/function exists with the relevant name.
       This can be checked by passing NULL in place of the types.
       If the form begins with 'core', though, skip this part. If
       any errors occur here, then pop them from the reporter and
       keep going - only if the rest of the function fails, should
       the errors be restored. */

    Error *backup_error = NULL;

    prefixed_with_core = !(t->str_value.compare("core"));

    if (!prefixed_with_core) {
        pr->type = NULL;
        bool res = parsePotentialProcCall(units, fn, block, n, get_address,
                                          wanted_type, pr, &backup_error);
        if (!res) {
            return false;
        } else if (pr->type) {
            return true;
        }
    } else {
        std::vector<Node *> *but_one = new std::vector<Node *>;
        but_one->insert(but_one->begin(), lst->begin() + 1, lst->end());
        lst = but_one;

        n = new Node(but_one);
        first = (*lst)[0];

        if (!first->is_token) {
            first = units->top()->mp->parsePotentialMacroCall(first);
            if (!first) {
                return false;
            }
        }
        if (!first->is_token) {
            Error *e = new Error(FirstListElementMustBeAtom, n);
            ctx->er->addError(e);
            return false;
        }

        t = first->token;
        if (t->type != TokenType::String) {
            Error *e = new Error(FirstListElementMustBeSymbol, n);
            ctx->er->addError(e);
            return false;
        }
    }

    /* Standard core forms. */

    standard_core_form_t core_fn =
        CoreForms::getStandard(t->str_value.c_str());
    if (core_fn) {
        return core_fn(units, fn, block, n,
                       get_address, prefixed_with_core, pr);
    }

    /* Macro core forms. */

    macro_core_form_t core_mac =
        CoreForms::getMacro(t->str_value.c_str());
    if (core_mac) {
        Node *new_node = core_mac(ctx, n);
        if (!new_node) {
            return false;
        }
        return FormProcInstParse(units, fn, block, new_node,
                                 get_address, false, wanted_type, pr);
    }

    /* Not core form/macro, nor function. If the string token is
     * 'destroy', then treat this as a no-op (because it's annoying to
     * have to check, in macros, whether destroy happens to be
     * implemented over a particular type). */

    if (!(t->str_value.compare("destroy"))) {
        pr->set(block, ctx->tr->type_void, NULL);
        return true;
    }

    /* If a backup error was set earlier, then return it now. */

    if (backup_error) {
        ctx->er->addError(backup_error);
        return false;
    }

    /* If nothing else is applicable: parse the first element of the
     * list.  If it is a function pointer, then go to funcall.  If it
     * is a struct, see if it is a function object. */

    int last_error_count =
        ctx->er->getErrorTypeCount(ErrorType::Error);
    ParseResult try_fp;
    bool res = FormProcInstParse(units, fn, block, (*lst)[0], get_address,
                                 false, wanted_type, &try_fp);
    if (!res) {
        /* If this fails, and there is one extra error, and the error
         * is 'variable not in scope', then change it to 'not in
         * scope' (it could be intended as either a variable, a macro
         * or a fn). */
        int new_error_count =
            ctx->er->getErrorTypeCount(ErrorType::Error);
        if (new_error_count == (last_error_count + 1)) {
            Error *e = ctx->er->popLastError();
            if (e->instance == VariableNotInScope) {
                e->instance = NotInScope;
            }
            ctx->er->addError(e);
        }
        return false;
    }

    block = try_fp.block;
    if (try_fp.type->points_to
            && try_fp.type->points_to->is_function) {
        Node *funcall_str_node = new Node("funcall");
        funcall_str_node->filename = ctx->er->current_filename;
        lst->insert(lst->begin(), funcall_str_node);
        bool res =
            FormProcFuncallParse(units, fn, block, n, get_address,
                                 false, pr);
        return res;
    }

    res = FormProcInstParse(units, fn, try_fp.block, (*lst)[0],
                            true, false, wanted_type, &try_fp);
    if (!res) {
        return false;
    }
    if (isFunctionObject(ctx, &try_fp)) {
        return parseFunctionObjectCall(units, fn, block, n, get_address,
                                       false, wanted_type, pr, &try_fp);
    }

    Error *e = new Error(NotInScope, n, t->str_value.c_str());
    ctx->er->addError(e);
    return false;
}

bool
FormProcInstParse(Units *units, Function *fn, llvm::BasicBlock *block,
                  Node *node, bool get_address, bool prefixed_with_core,
                  Type *wanted_type, ParseResult *pr, bool no_copy)
{
    bool res =
        parseInternal(units, fn, block, node, get_address,
                      prefixed_with_core, wanted_type, pr);

    if (!res) {
        return false;
    }
    if (fn->is_setf_fn) {
        return true;
    }
    if (!no_copy) {
        Operation::Copy(units->top()->ctx, fn, pr, pr);
    }

    return true;
}
}
