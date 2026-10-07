#include "SemanticQuery.hpp"
#include "ASTVisitor.h"
#include "Parser.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>

// A module's name as the AST records it, and as the user wrote it. Inside a
// package build the AST names a dependency's module `<package>/<path>`
// (`dep/dep`), so two packages' modules of the same name stay apart (LEC-001);
// what is written in the source -- what an editor must measure for a token or a
// hover range, and print -- is the part before the slash. For every other
// module the two are the same.
static std::string spelledModule(const std::string& identity) {
    auto slash = identity.find('/');
    return slash == std::string::npos ? identity : identity.substr(0, slash);
}

// Shared with the completion-context scanning further down this file.
static bool isIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// FuncDefExprAST::getName() returns the qualified "Struct.method" for a
// struct method declaration (the flat name the resolver registers it
// under), but getNameLoc() deliberately points at just the bare method
// identifier's own token -- see its doc comment in ASTDecl.h ("Repoint
// NameLoc at the METHOD name specifically", issue #230). Pairing
// getNameLoc() with getName().length() as a span therefore overshoots the
// real identifier into whatever follows it in source (e.g. "new(x: do" for
// "fn Point.new(x: double...)" -- 9 characters from "Point.new", starting
// at "new"'s real column). This recovers just the bare name's length: the
// substring after the last '.', or the whole name when there isn't one (an
// ordinary free function, where getName() is already bare).
static int bareNameLength(const std::string &qualifiedName) {
    auto dot = qualifiedName.find_last_of('.');
    return static_cast<int>(dot == std::string::npos
                                 ? qualifiedName.length()
                                 : qualifiedName.length() - dot - 1);
}

// A visitor that traverses the AST to find the tightest node enclosing a location.
class NodeFinderVisitor : public ASTVisitor {
    int TargetLine;
    int TargetCol;
    const ExprAST* BestNode = nullptr;

    bool contains(const SourceLocation& Loc, int Length) const {
        if (Loc.Line != TargetLine) return false;
        return TargetCol >= Loc.Col && TargetCol < Loc.Col + Length;
    }

    void checkNode(const ExprAST* Node, const SourceLocation& Loc, int Length) {
        if (contains(Loc, Length)) {
            BestNode = Node;
        }
    }

public:
    NodeFinderVisitor(int line, int col) : TargetLine(line), TargetCol(col) {}

    const ExprAST* getBestNode() const { return BestNode; }

    void visitNumber(NumberExprAST &Node) override {}
    void visitBool(BoolExprAST &Node) override {}
    void visitString(StringExprAST &Node) override {}
    void visitNull(NullExprAST &Node) override {}

    void visitVariable(VariableExprAST &Node) override {
        checkNode(&Node, Node.getLoc(), Node.getName().length());
    }
    
    void visitVarDecl(VarDeclExprAST &Node) override {
        checkNode(&Node, Node.getNameLoc(), static_cast<int>(Node.getName().length()));
        if (Node.getInitializer()) {
            const_cast<ExprAST*>(Node.getInitializer())->accept(*this);
        }
    }

    void visitBinary(BinaryExprAST &Node) override {
        if (Node.getLHS()) const_cast<ExprAST*>(Node.getLHS())->accept(*this);
        if (Node.getRHS()) const_cast<ExprAST*>(Node.getRHS())->accept(*this);
    }
    
    void visitUnary(UnaryExprAST &Node) override {
        if (Node.getOperand()) const_cast<ExprAST*>(Node.getOperand())->accept(*this);
    }
    
    void visitAddressOf(AddressOfExprAST &Node) override {
        if (Node.getOperand()) const_cast<ExprAST*>(Node.getOperand())->accept(*this);
    }
    
    void visitAssignment(AssignmentExprAST &Node) override {
        if (Node.getLHS()) const_cast<ExprAST*>(Node.getLHS())->accept(*this);
        if (Node.getRHS()) const_cast<ExprAST*>(Node.getRHS())->accept(*this);
    }
    
    void visitCompoundAssignment(CompoundAssignmentExprAST &Node) override {
        if (Node.getLHS()) const_cast<ExprAST*>(Node.getLHS())->accept(*this);
        if (Node.getRHS()) const_cast<ExprAST*>(Node.getRHS())->accept(*this);
    }
    
    void visitPostfixExpr(PostfixExprAST &Node) override {
        if (Node.getOperand()) const_cast<ExprAST*>(Node.getOperand())->accept(*this);
    }
    
    void visitCast(CastExprAST &Node) override {
        if (Node.getOperand()) const_cast<ExprAST*>(Node.getOperand())->accept(*this);
    }

    void visitBlock(BlockExprAST &Node) override {
        for (const auto& expr : Node.getExpressions()) {
            if (expr) const_cast<ExprAST*>(expr.get())->accept(*this);
        }
    }
    
    void visitIf(IfExprAST &Node) override {
        for (const auto& branch : Node.getBranches()) {
            if (branch.Cond) const_cast<ExprAST*>(branch.Cond.get())->accept(*this);
            if (branch.Body) const_cast<ExprAST*>(branch.Body.get())->accept(*this);
        }
        if (Node.getElseBody()) const_cast<ExprAST*>(Node.getElseBody())->accept(*this);
    }
    
    void visitWhile(WhileExprAST &Node) override {
        if (Node.getCond()) const_cast<ExprAST*>(Node.getCond())->accept(*this);
        if (Node.getBody()) const_cast<ExprAST*>(Node.getBody())->accept(*this);
    }
    
    void visitFor(ForExprAST &Node) override {
        if (Node.getStart()) const_cast<ExprAST*>(Node.getStart())->accept(*this);
        if (Node.getEnd()) const_cast<ExprAST*>(Node.getEnd())->accept(*this);
        if (Node.getStep()) const_cast<ExprAST*>(Node.getStep())->accept(*this);
        if (Node.getBody()) const_cast<ExprAST*>(Node.getBody())->accept(*this);
    }
    
    void visitBreak(BreakExprAST &Node) override {}
    void visitContinue(ContinueExprAST &Node) override {}
    
    void visitReturn(ReturnExprAST &Node) override {
        if (Node.getVal()) const_cast<ExprAST*>(Node.getVal())->accept(*this);
    }

    void visitFuncDef(FuncDefExprAST &Node) override {
        checkNode(&Node, Node.getNameLoc(), bareNameLength(Node.getName()));
        if (Node.getBodyMut()) const_cast<ExprAST*>(Node.getBodyMut())->accept(*this);
    }

    void visitFuncCall(FuncCallExprAST &Node) override {
        if (Node.getCallee()) const_cast<ExprAST*>(Node.getCallee())->accept(*this);
        // `Module::func(...)` / `StructName::method(...)`: Resolver sets
        // ResolvedCallTarget on THIS (FuncCallExprAST) node, not on the
        // ScopeAccessExprAST callee -- visitScopeAccess (below) would
        // otherwise leave the callee itself as BestNode when the cursor is
        // on the right-hand name, which getDeclarationLoc()/hoverText()
        // don't know how to read a resolved call target off of. Runs AFTER
        // the recursive accept() above so it wins the overwrite.
        if (auto scopeCallee = dynamic_cast<const ScopeAccessExprAST*>(Node.getCallee())) {
            if (contains(scopeCallee->getRightLoc(), static_cast<int>(scopeCallee->getRightName().length()))) {
                BestNode = &Node;
            }
        } else if (auto varCallee = dynamic_cast<const VariableExprAST*>(Node.getCallee())) {
            // A bare `add(...)` callee is a VariableExprAST whose own
            // ResolvedSymbol is null -- it names a FUNCTION, not a real
            // variable binding, so visitVariable's ordinary checkNode()
            // above already made it BestNode purely by position, but
            // hoverText()/getDeclarationLoc() can't read a call target off
            // a null-symbol variable. Retarget to the FuncCallExprAST
            // itself (which DOES carry getResolvedCallTarget()) so a plain
            // free-function call hovers/jumps exactly like a module- or
            // struct-qualified one already does above.
            if (!varCallee->getResolvedSymbol() &&
                contains(varCallee->getLoc(), static_cast<int>(varCallee->getName().length()))) {
                BestNode = &Node;
            }
        }
        for (const auto& arg : Node.getArgs()) {
            if (arg) const_cast<ExprAST*>(arg.get())->accept(*this);
        }
    }

    void visitMethodCall(MethodCallExprAST &Node) override {
        if (Node.getBase()) const_cast<ExprAST*>(Node.getBase())->accept(*this);
        checkNode(&Node, Node.getNameLoc(), static_cast<int>(Node.getMethod().length()));
        for (const auto& arg : Node.getArgs()) {
            if (arg) const_cast<ExprAST*>(arg.get())->accept(*this);
        }
    }

    void visitFieldAccess(FieldAccessExprAST &Node) override {
        if (Node.getBase()) const_cast<ExprAST*>(Node.getBase())->accept(*this);
        checkNode(&Node, Node.getNameLoc(), static_cast<int>(Node.getFieldName().length()));
    }

    void visitScopeAccess(ScopeAccessExprAST &Node) override {
        checkNode(&Node, Node.getLeftLoc(), spelledModule(Node.getLeftName()).length());
        checkNode(&Node, Node.getRightLoc(), Node.getRightName().length());
    }

    void visitStructDecl(StructDeclExprAST &Node) override {
        checkNode(&Node, Node.getNameLoc(), static_cast<int>(Node.getName().length()));
    }

    void visitArrayAccess(ArrayAccessExprAST &Node) override {
        if (Node.getBase()) const_cast<ExprAST*>(Node.getBase())->accept(*this);
        if (Node.getIndex()) const_cast<ExprAST*>(Node.getIndex())->accept(*this);
    }

    // Only the `import <name>` statement's own name is hit-testable here --
    // NOT Node.getBody(), the full parsed AST of a DIFFERENT file (its
    // loc/col coordinates belong to that file, not this one; recursing
    // would let a hover/definition request land on the wrong document's
    // positions). Same reasoning SemanticTokensVisitor::visitModuleImport
    // already documents.
    void visitModuleImport(ModuleImportExprAST &Node) override {
        checkNode(&Node, Node.getNameLoc(), static_cast<int>(spelledModule(Node.getModuleName()).length()));
    }

    // Default implementations for others that we might not fully traverse for MVP
    // but we can add more if needed
};

const ExprAST* SemanticQuery::findNodeAt(const DocumentAnalysis& analysis, int line, int col) {
    NodeFinderVisitor visitor(line, col);
    // Skip the synthetic prelude imports (see DocumentAnalysis::
    // preludeImportCount's doc comment) -- they're not real source the
    // user can click on.
    for (size_t i = analysis.preludeImportCount(); i < analysis.asts().size(); ++i) {
        if (analysis.asts()[i]) {
            const_cast<ExprAST*>(analysis.asts()[i].get())->accept(visitor);
        }
    }
    return visitor.getBestNode();
}

std::optional<SymbolLocation> SemanticQuery::getDeclarationLoc(const ExprAST* node) {
    if (!node) return std::nullopt;

    if (auto varNode = dynamic_cast<const VariableExprAST*>(node)) {
        if (auto sym = varNode->getResolvedSymbol()) {
            return SymbolLocation{sym->DeclLoc, "", static_cast<int>(sym->Name.length())};
        }
    } else if (auto varDecl = dynamic_cast<const VarDeclExprAST*>(node)) {
        if (auto sym = varDecl->getResolvedSymbol()) {
            return SymbolLocation{sym->DeclLoc, "", static_cast<int>(sym->Name.length())};
        }
    } else if (auto callNode = dynamic_cast<const FuncCallExprAST*>(node)) {
        if (auto target = callNode->getResolvedCallTarget()) {
            // bareNameLength(), not a raw .length(): a struct method's
            // BaseName/getName() can be the qualified "Struct.method" (same
            // reasoning as bareNameLength()'s own doc comment above), but
            // only the bare "method" identifier appears at the DECLARATION
            // site this length is measuring.
            return SymbolLocation{target->DeclLoc, target->SourceModule, bareNameLength(target->BaseName)};
        }
    } else if (auto fnDef = dynamic_cast<const FuncDefExprAST*>(node)) {
        if (auto target = fnDef->getResolvedDef()) {
            return SymbolLocation{target->DeclLoc, target->SourceModule, bareNameLength(target->BaseName)};
        }
    } else if (auto methodNode = dynamic_cast<const MethodCallExprAST*>(node)) {
        if (auto target = methodNode->getResolvedCallTarget()) {
            return SymbolLocation{target->DeclLoc, target->SourceModule, bareNameLength(target->BaseName)};
        }
    }
    return std::nullopt;
}

namespace {
// The type of the VALUE an expression evaluates to, for the small set of
// expression kinds hover needs to chase a field access through (`p.x`,
// `a.b.c`, `f().field`). Deliberately conservative -- returns nullptr
// (never a guess) for anything it doesn't specifically handle, e.g. a
// binary expression or a literal. Not a general type-inference engine: it
// only reads data the Resolver/TypeChecker already attached to the node.
TurfTypePtr exprType(const ExprAST* e) {
    if (!e) return nullptr;
    if (auto v = dynamic_cast<const VariableExprAST*>(e)) {
        auto sym = v->getResolvedSymbol();
        return sym ? sym->Type : nullptr;
    }
    if (auto fa = dynamic_cast<const FieldAccessExprAST*>(e)) {
        auto baseTy = exprType(fa->getBase());
        auto st = dynamic_cast<const TurfStructType*>(baseTy.get());
        return st ? st->getFieldType(fa->getFieldName()) : nullptr;
    }
    if (auto mc = dynamic_cast<const MethodCallExprAST*>(e)) {
        auto target = mc->getResolvedCallTarget();
        return target ? target->ReturnType : nullptr;
    }
    if (auto fc = dynamic_cast<const FuncCallExprAST*>(e)) {
        auto target = fc->getResolvedCallTarget();
        return target ? target->ReturnType : nullptr;
    }
    return nullptr;
}
} // namespace

std::optional<HoverInfo> SemanticQuery::hover(const DocumentAnalysis& analysis, const ExprAST* node,
                                              int cursorLine, int cursorCol) {
    if (!node) return std::nullopt;

    if (auto varNode = dynamic_cast<const VariableExprAST*>(node)) {
        if (auto sym = varNode->getResolvedSymbol()) {
            std::string text = "```turf\n";
            text += (sym->IsImmutable ? "immut " : "mut ");
            text += sym->Name + ": ";
            text += (sym->Type ? sym->Type->getName() : "unknown");
            text += "\n```";
            if (sym->IsParameter) text += "\nparameter";
            return HoverInfo{text, varNode->getLoc(), static_cast<int>(varNode->getName().length())};
        }
    } else if (auto varDecl = dynamic_cast<const VarDeclExprAST*>(node)) {
        if (auto sym = varDecl->getResolvedSymbol()) {
            std::string text = "```turf\n";
            text += (sym->IsImmutable ? "immut " : "mut ");
            text += sym->Name + ": ";
            text += (sym->Type ? sym->Type->getName() : "unknown");
            text += "\n```";
            return HoverInfo{text, varDecl->getNameLoc(), static_cast<int>(varDecl->getName().length())};
        }
    } else if (auto callNode = dynamic_cast<const FuncCallExprAST*>(node)) {
        if (auto target = callNode->getResolvedCallTarget()) {
            SourceLocation loc = callNode->getLoc();
            int len = static_cast<int>(target->BaseName.length());
            // `Module::func(...)`: the callee is a ScopeAccessExprAST and the
            // cursor was actually over ITS right-hand name (see
            // NodeFinderVisitor::visitFuncCall's identical special-case) --
            // use that name's real span instead of the whole call's Loc.
            if (auto scopeCallee = dynamic_cast<const ScopeAccessExprAST*>(callNode->getCallee())) {
                loc = scopeCallee->getRightLoc();
                len = static_cast<int>(scopeCallee->getRightName().length());
            } else if (auto varCallee = dynamic_cast<const VariableExprAST*>(callNode->getCallee())) {
                loc = varCallee->getLoc();
                len = static_cast<int>(varCallee->getName().length());
            }
            return HoverInfo{"```turf\nfn " + target->signature() + "\n```", loc, len};
        }
    } else if (auto fnDef = dynamic_cast<const FuncDefExprAST*>(node)) {
        if (auto target = fnDef->getResolvedDef()) {
            // Overload listing (doc 06 edge case): getResolvedDef() only
            // carries THIS declaration's own overload, but a bare function
            // NAME (as opposed to a specific call, which already resolved
            // to exactly one via getResolvedCallTarget() above) is
            // ambiguous about which one the user means -- list every
            // overload sharing this flat name in the current file (an
            // imported module's own FuncDefExprAST is never hit-tested by
            // findNodeAt() -- ModuleImportExprAST bodies aren't recursed
            // into for position lookups -- so "current file" is the only
            // case this can ever see).
            std::string text;
            std::vector<const ResolverFunctionSymbol*> overloads;
            overloads = analysis.symbols().lookupCurrentFileFunctions(fnDef->getName());
            if (overloads.size() > 1) {
                text = "```turf\n";
                for (size_t i = 0; i < overloads.size(); ++i) {
                    if (i) text += "\n";
                    text += "fn " + overloads[i]->signature();
                }
                text += "\n```";
            } else {
                text = "```turf\nfn " + target->signature() + "\n```";
            }
            return HoverInfo{text, fnDef->getNameLoc(), bareNameLength(fnDef->getName())};
        }
    } else if (auto methodNode = dynamic_cast<const MethodCallExprAST*>(node)) {
        if (auto target = methodNode->getResolvedCallTarget()) {
            return HoverInfo{"```turf\nfn " + target->signature() + "\n```",
                             methodNode->getNameLoc(), static_cast<int>(methodNode->getMethod().length())};
        }
    } else if (auto structDecl = dynamic_cast<const StructDeclExprAST*>(node)) {
        std::string text = "```turf\nstruct " + structDecl->getName() + " {\n";
        for (const auto& field : structDecl->getFields()) {
            text += "    " + field.Name + ": " + (field.Type ? field.Type->getName() : "unknown") + "\n";
        }
        text += "}\n```";
        return HoverInfo{text, structDecl->getNameLoc(), static_cast<int>(structDecl->getName().length())};
    } else if (auto fieldAccess = dynamic_cast<const FieldAccessExprAST*>(node)) {
        // `p.x` -- resolve p's type, then look the field up on it directly
        // via TurfStructType::getFieldType(), which needs no AST-level
        // struct-decl lookup (the type system already carries field data).
        auto baseTy = exprType(fieldAccess->getBase());
        if (auto st = dynamic_cast<const TurfStructType*>(baseTy.get())) {
            if (auto fieldTy = st->getFieldType(fieldAccess->getFieldName())) {
                std::string text = "```turf\n" + fieldAccess->getFieldName() + ": " +
                                    fieldTy->getName() + "\n```\nfield of `" + st->getName() + "`";
                return HoverInfo{text, fieldAccess->getNameLoc(),
                                 static_cast<int>(fieldAccess->getFieldName().length())};
            }
        }
    } else if (auto scopeAccess = dynamic_cast<const ScopeAccessExprAST*>(node)) {
        // Left::Right is ONE node for both names -- the same node type
        // NodeFinderVisitor hit-tests both LeftLoc and RightLoc against, so
        // there's no way to tell which side the cursor is actually over
        // without redoing that same comparison here.
        bool onRight = scopeAccess->getRightLoc().Line == cursorLine &&
                       cursorCol >= scopeAccess->getRightLoc().Col &&
                       cursorCol < scopeAccess->getRightLoc().Col +
                                       static_cast<int>(scopeAccess->getRightName().length());
        if (onRight) {
            if (scopeAccess->ResolvedKind == ScopeAccessKind::EnumVariant) {
                std::string text = "`" + scopeAccess->getRightName() +
                                   "` — variant of enum `" + scopeAccess->getLeftName() + "`";
                return HoverInfo{text, scopeAccess->getRightLoc(),
                                 static_cast<int>(scopeAccess->getRightName().length())};
            }
            // A static method / module function / module constant / etc:
            // answering this properly needs a symbol-table lookup hover()
            // doesn't have access to (no `analysis` parameter) -- return no
            // hover rather than a guess, per this function's own edge-case
            // policy (see doc 06's "unresolved node" rule).
            return std::nullopt;
        }
        const std::string spelled = spelledModule(scopeAccess->getLeftName());
        std::string text = "module `" + spelled + "`";
        return HoverInfo{text, scopeAccess->getLeftLoc(), static_cast<int>(spelled.length())};
    } else if (auto moduleImport = dynamic_cast<const ModuleImportExprAST*>(node)) {
        const std::string spelled = spelledModule(moduleImport->getModuleName());
        std::string text = "module `" + spelled + "`";
        if (!moduleImport->getResolvedPath().empty())
            text += "\n\n" + moduleImport->getResolvedPath();
        return HoverInfo{text, moduleImport->getNameLoc(), static_cast<int>(spelled.length())};
    }
    return std::nullopt;
}

namespace {
// Indexes every struct/enum NAME visible from the top level of a document
// -- declared directly, or in any transitively-imported module -- to its
// declaration location and owning module name (mirrors
// ResolverTypeRegistry's own flat, module-blind struct/enum namespace on
// the compiler side, but built from the already-parsed AST instead of
// needing a new query surface there: an imported module's full AST is
// already sitting in its ModuleImportExprAST::getBody(), so no re-analysis
// of the target file is needed). First declaration of a name wins (structs
///enums share ONE flat namespace, so a genuine same-name collision across
// modules is itself a compile error the Resolver already reports).
class TypeDeclIndexVisitor : public ASTVisitor {
public:
    // `length` is the span to highlight at `loc` -- the real name length
    // for a struct (loc IS the name), but deliberately just 1 (a caret) for
    // an enum: loc there is the `enum` KEYWORD (see visitEnumDecl's doc
    // comment), so highlighting typeName.length() characters from that
    // position would bleed into "enum " + part of the name instead of
    // landing on the name itself.
    struct Entry { SourceLocation loc; std::string moduleName; int length; };
    std::unordered_map<std::string, Entry> types;

    void visitStructDecl(StructDeclExprAST &Node) override {
        types.emplace(Node.getName(),
                      Entry{Node.getNameLoc(), currentModule, static_cast<int>(Node.getName().length())});
    }
    // EnumDeclExprAST has no separate NameLoc (unlike StructDeclExprAST --
    // it predates issue #230's name-location fix), so this lands on the
    // `enum` keyword rather than the name itself -- still a correct jump
    // target, just a less precise one than the struct case.
    void visitEnumDecl(EnumDeclExprAST &Node) override {
        types.emplace(Node.getName(), Entry{Node.getLoc(), currentModule, 1});
    }
    void visitModuleImport(ModuleImportExprAST &Node) override {
        if (!Node.getBody()) return;
        std::string saved = currentModule;
        currentModule = Node.getModuleName();
        const_cast<ExprAST*>(Node.getBody())->accept(*this);
        currentModule = saved;
    }
    // A module's body is a single BlockExprAST wrapping its own top-level
    // statements (structs, fns, nested imports, ...) -- without this
    // override the default (no-op) ASTVisitor base swallows the whole
    // import's contents, exactly the bug DocumentIndexVisitor's identical
    // override (above) already works around for completion.
    void visitBlock(BlockExprAST &Node) override {
        for (const auto& e : Node.getExpressions())
            if (e) const_cast<ExprAST*>(e.get())->accept(*this);
    }
private:
    std::string currentModule; // "" while walking the top-level file itself
};
} // namespace

std::optional<SymbolLocation> SemanticQuery::getTypeDefinitionLoc(const DocumentAnalysis& analysis,
                                                                   const ExprAST* node) {
    auto ty = exprType(node);
    if (!ty) {
        // exprType() only understands VariableExprAST/FieldAccessExprAST/
        // MethodCallExprAST/FuncCallExprAST as a base -- a bare VarDeclExprAST
        // (the declaration site itself, not a use) carries its type directly
        // on the resolved symbol instead.
        if (auto varDecl = dynamic_cast<const VarDeclExprAST*>(node)) {
            if (auto sym = varDecl->getResolvedSymbol()) ty = sym->Type;
        }
    }
    if (!ty) return std::nullopt;

    TypeDeclIndexVisitor indexer;
    // Skip the synthetic prelude imports (DocumentAnalysis::
    // preludeImportCount): walking them is harmless (no position collision
    // concern here, unlike semantic tokens), but there's nothing worth
    // finding inside them for THIS lookup either -- omitted for clarity.
    for (size_t i = analysis.preludeImportCount(); i < analysis.asts().size(); ++i) {
        if (analysis.asts()[i]) const_cast<ExprAST*>(analysis.asts()[i].get())->accept(indexer);
    }

    // A generic instantiation's getName() includes its type arguments
    // (e.g. "List<int>" for GenericType), but the decl index is keyed by
    // the bare declared name ("List", from StructDeclExprAST::getName(),
    // which never includes TypeParams -- those are tracked separately via
    // getTypeParams()) -- strip everything from the first '<' before
    // looking up.
    std::string typeName = ty->getName();
    if (auto lt = typeName.find('<'); lt != std::string::npos) typeName.resize(lt);

    auto it = indexer.types.find(typeName);
    if (it == indexer.types.end()) return std::nullopt;
    ResolverSourceLocation loc{it->second.loc.Line, it->second.loc.Col};
    return SymbolLocation{loc, it->second.moduleName, it->second.length};
}

class SemanticTokensVisitor : public ASTVisitor {
public:
    std::vector<SemanticToken> tokens;

    void addToken(const SourceLocation& loc, int length, SemanticTokenType type, int modifiers = 0) {
        if (loc.Line > 0 && length > 0) {
            tokens.push_back({loc.Line, loc.Col, length, type, modifiers});
        }
    }

    void visitVariable(VariableExprAST &Node) override {
        int modifiers = 0;
        SemanticTokenType type = SemanticTokenType::Variable;
        if (auto sym = Node.getResolvedSymbol()) {
            if (sym->IsImmutable) modifiers |= static_cast<int>(SemanticTokenModifier::Readonly);
            // A parameter is just a pre-bound local (see VariableSymbol::
            // IsParameter's doc comment in turf-lang) -- color its USES
            // distinctly from an ordinary local, matching the Parameter
            // token already emitted at its declaration site in visitFuncDef.
            if (sym->IsParameter) type = SemanticTokenType::Parameter;
        }
        addToken(Node.getLoc(), Node.getName().length(), type, modifiers);
    }
    
    void visitVarDecl(VarDeclExprAST &Node) override {
        int modifiers = static_cast<int>(SemanticTokenModifier::Declaration);
        if (Node.isImmutable()) {
            modifiers |= static_cast<int>(SemanticTokenModifier::Readonly);
        }
        addToken(Node.getNameLoc(), Node.getName().length(), SemanticTokenType::Variable, modifiers);
        // Type annotation (`: int` in `mut x: int = 5;`) -- turf-lang's
        // parser started recording this span on VarDeclExprAST specifically
        // so tooling could color it (TurfType itself has no source location
        // of its own; see VarDeclExprAST::TypeLoc's doc comment). Length is
        // the type's rendered name, which matches source for every type this
        // can currently be (primitives, struct/enum names, generics written
        // out in full like `List<int>` -- there's no shorthand spelling that
        // could disagree with getName()).
        if (Node.getType())
            addToken(Node.getTypeLoc(), Node.getType()->getName().length(), SemanticTokenType::Type);
        if (Node.getInitializer()) {
            const_cast<ExprAST*>(Node.getInitializer())->accept(*this);
        }
    }

    void visitFuncDef(FuncDefExprAST &Node) override {
        int modifiers = static_cast<int>(SemanticTokenModifier::Declaration);
        addToken(Node.getNameLoc(), bareNameLength(Node.getName()), SemanticTokenType::Function, modifiers);
        // Parameters: name as Parameter, declared type as Type. Skips the
        // synthetic 'self' param automatically -- its Loc/TypeLoc are both
        // the default {0,0} (see ParamDecl's doc comment), and addToken()
        // already guards `loc.Line > 0`.
        for (const auto &Param : Node.getParams()) {
            addToken(Param.Loc, Param.Name.length(), SemanticTokenType::Parameter);
            if (Param.Type)
                addToken(Param.TypeLoc, Param.Type->getName().length(), SemanticTokenType::Type);
        }
        if (Node.getBodyMut()) const_cast<ExprAST*>(Node.getBodyMut())->accept(*this);
    }
    
    void visitFuncCall(FuncCallExprAST &Node) override {
        if (auto calleeVar = dynamic_cast<const VariableExprAST*>(Node.getCallee())) {
            addToken(calleeVar->getLoc(), calleeVar->getName().length(), SemanticTokenType::Function);
        } else if (Node.getCallee()) {
            const_cast<ExprAST*>(Node.getCallee())->accept(*this);
        }
        for (const auto& arg : Node.getArgs()) {
            if (arg) const_cast<ExprAST*>(arg.get())->accept(*this);
        }
    }

    void visitBlock(BlockExprAST &Node) override {
        for (const auto& expr : Node.getExpressions()) {
            if (expr) const_cast<ExprAST*>(expr.get())->accept(*this);
        }
    }

    void visitIf(IfExprAST &Node) override {
        for (const auto& branch : Node.getBranches()) {
            if (branch.Cond) const_cast<ExprAST*>(branch.Cond.get())->accept(*this);
            if (branch.Body) const_cast<ExprAST*>(branch.Body.get())->accept(*this);
        }
        if (Node.getElseBody()) const_cast<ExprAST*>(Node.getElseBody())->accept(*this);
    }
    
    void visitWhile(WhileExprAST &Node) override {
        if (Node.getCond()) const_cast<ExprAST*>(Node.getCond())->accept(*this);
        if (Node.getBody()) const_cast<ExprAST*>(Node.getBody())->accept(*this);
    }
    
    void visitFor(ForExprAST &Node) override {
        if (Node.getStart()) const_cast<ExprAST*>(Node.getStart())->accept(*this);
        if (Node.getEnd()) const_cast<ExprAST*>(Node.getEnd())->accept(*this);
        if (Node.getStep()) const_cast<ExprAST*>(Node.getStep())->accept(*this);
        if (Node.getBody()) const_cast<ExprAST*>(Node.getBody())->accept(*this);
    }

    void visitBinary(BinaryExprAST &Node) override {
        if (Node.getLHS()) const_cast<ExprAST*>(Node.getLHS())->accept(*this);
        if (Node.getRHS()) const_cast<ExprAST*>(Node.getRHS())->accept(*this);
    }
    
    void visitAssignment(AssignmentExprAST &Node) override {
        if (Node.getLHS()) const_cast<ExprAST*>(Node.getLHS())->accept(*this);
        if (Node.getRHS()) const_cast<ExprAST*>(Node.getRHS())->accept(*this);
    }

    void visitCompoundAssignment(CompoundAssignmentExprAST &Node) override {
        if (Node.getLHS()) const_cast<ExprAST*>(Node.getLHS())->accept(*this);
        if (Node.getRHS()) const_cast<ExprAST*>(Node.getRHS())->accept(*this);
    }

    void visitUnary(UnaryExprAST &Node) override {
        if (Node.getOperand()) const_cast<ExprAST*>(Node.getOperand())->accept(*this);
    }

    void visitAddressOf(AddressOfExprAST &Node) override {
        if (Node.getOperand()) const_cast<ExprAST*>(Node.getOperand())->accept(*this);
    }

    void visitCast(CastExprAST &Node) override {
        if (Node.getOperand()) const_cast<ExprAST*>(Node.getOperand())->accept(*this);
    }

    void visitPostfixExpr(PostfixExprAST &Node) override {
        if (Node.getOperand()) const_cast<ExprAST*>(Node.getOperand())->accept(*this);
    }

    void visitReturn(ReturnExprAST &Node) override {
        if (Node.getVal()) const_cast<ExprAST*>(Node.getVal())->accept(*this);
    }

    void visitCompare(CompareExprAST &Node) override {
        if (Node.getSubject()) const_cast<ExprAST*>(Node.getSubject())->accept(*this);
        for (const auto& arm : Node.getArms()) {
            for (const auto& val : arm.Values) {
                if (val) const_cast<ExprAST*>(val.get())->accept(*this);
            }
            if (arm.Body) const_cast<ExprAST*>(arm.Body.get())->accept(*this);
        }
        if (Node.getOtherBody()) const_cast<ExprAST*>(Node.getOtherBody())->accept(*this);
    }

    // Struct name at its declaration site, plus each field's own name
    // (Property, declaration) and declared type (Type). Field types aren't
    // separate ExprAST nodes -- StructField is a plain name+TurfTypePtr
    // pair -- but it now also carries NameLoc/TypeLoc spans (turf-lang
    // added these specifically for this), so there's real data to emit
    // here even without a node to recurse into.
    void visitStructDecl(StructDeclExprAST &Node) override {
        addToken(Node.getNameLoc(), Node.getName().length(), SemanticTokenType::Struct,
                 static_cast<int>(SemanticTokenModifier::Declaration));
        for (const auto &Field : Node.getFields()) {
            addToken(Field.NameLoc, Field.Name.length(), SemanticTokenType::Property,
                     static_cast<int>(SemanticTokenModifier::Declaration));
            if (Field.Type)
                addToken(Field.TypeLoc, Field.Type->getName().length(), SemanticTokenType::Type);
        }
    }

    // Base.field -- Base gets whatever token its own shape implies (variable,
    // another field access, a call result, ...); FieldName is always a
    // Property token positioned right after Base ends. FieldAccessExprAST
    // doesn't carry its own loc for just the field name, so the field
    // token's column is derived from the base's end (Base.getLoc().Col +
    // Base's own rendered length) + 1 for the '.' -- approximated here using
    // FieldName's own reported Loc, which the parser sets to the field
    // identifier's real position.
    void visitFieldAccess(FieldAccessExprAST &Node) override {
        if (Node.getBase()) const_cast<ExprAST*>(Node.getBase())->accept(*this);
        addToken(Node.getNameLoc(), Node.getFieldName().length(), SemanticTokenType::Property);
    }

    // Base.method(args) -- Method is highlighted distinctly from a Property
    // access and from a free Function call.
    void visitMethodCall(MethodCallExprAST &Node) override {
        if (Node.getBase()) const_cast<ExprAST*>(Node.getBase())->accept(*this);
        addToken(Node.getNameLoc(), Node.getMethod().length(), SemanticTokenType::Method);
        for (const auto& arg : Node.getArgs()) {
            if (arg) const_cast<ExprAST*>(arg.get())->accept(*this);
        }
    }

    // Left::Right -- Left is a module/struct/enum name (Namespace); Right's
    // kind depends on what the Resolver decided Left actually is (an enum
    // variant reads better as EnumMember than as a generic Method/Function).
    void visitScopeAccess(ScopeAccessExprAST &Node) override {
        addToken(Node.getLeftLoc(), spelledModule(Node.getLeftName()).length(), SemanticTokenType::Namespace);
        SemanticTokenType rightType = (Node.ResolvedKind == ScopeAccessKind::EnumVariant)
            ? SemanticTokenType::EnumMember
            : SemanticTokenType::Method;
        addToken(Node.getRightLoc(), Node.getRightName().length(), rightType);
    }

    // Only the `import <name>` statement itself is tokenized here -- NOT
    // Node.getBody(), which is the full parsed AST of a DIFFERENT file
    // (loc/col coordinates are relative to that file, not this document; a
    // recursive visit here would corrupt this document's token stream with
    // positions that belong to someone else's file).
    void visitModuleImport(ModuleImportExprAST &Node) override {
        addToken(Node.getNameLoc(), spelledModule(Node.getModuleName()).length(), SemanticTokenType::Namespace,
                 static_cast<int>(SemanticTokenModifier::Declaration));
    }

    void visitExternFuncDecl(ExternFuncDeclExprAST &Node) override {
        addToken(Node.getNameLoc(), Node.getName().length(), SemanticTokenType::Function,
                 static_cast<int>(SemanticTokenModifier::Declaration));
    }

    void visitArrayDecl(ArrayDeclExprAST &Node) override {
        int modifiers = static_cast<int>(SemanticTokenModifier::Declaration);
        if (Node.isImmutable()) modifiers |= static_cast<int>(SemanticTokenModifier::Readonly);
        addToken(Node.getLoc(), Node.getName().length(), SemanticTokenType::Variable, modifiers);
        for (const auto& elem : Node.getInitList()) {
            if (elem) const_cast<ExprAST*>(elem.get())->accept(*this);
        }
    }

    void visitArrayLiteral(ArrayLiteralExprAST &Node) override {
        for (const auto& elem : Node.getElements()) {
            if (elem) const_cast<ExprAST*>(elem.get())->accept(*this);
        }
    }

    void visitArrayAccess(ArrayAccessExprAST &Node) override {
        if (Node.getBase()) const_cast<ExprAST*>(Node.getBase())->accept(*this);
        if (Node.getIndex()) const_cast<ExprAST*>(Node.getIndex())->accept(*this);
    }
};

#include <algorithm>

std::vector<SemanticToken> SemanticQuery::getSemanticTokens(const DocumentAnalysis& analysis) {
    SemanticTokensVisitor visitor;
    // Skip the synthetic prelude imports (DocumentAnalysis::
    // preludeImportCount) -- tokenizing them would emit up to 6 phantom
    // Namespace tokens overlapping the user's real first line.
    for (size_t i = analysis.preludeImportCount(); i < analysis.asts().size(); ++i) {
        if (analysis.asts()[i]) {
            const_cast<ExprAST*>(analysis.asts()[i].get())->accept(visitor);
        }
    }
    
    auto tokens = std::move(visitor.tokens);
    std::sort(tokens.begin(), tokens.end(), [](const SemanticToken& a, const SemanticToken& b) {
        if (a.line != b.line) return a.line < b.line;
        return a.col < b.col;
    });
    return tokens;
}

// ─── References / document highlight (doc 08) ──────────────────────────────

namespace {
// Collects ReferenceOccurrence for the 5 node kinds getDeclarationLoc()
// already resolves (variable use/decl, function call/def, method call) --
// deliberately NOT struct/enum/field/module-name occurrences yet (a real,
// documented scope limit, not an oversight -- see the header's own note).
// Traversal skeleton is otherwise identical to SemanticTokensVisitor's
// (every expression kind needs its own override, or the default no-op
// ASTVisitor base silently drops everything nested inside it).
class ReferenceVisitor : public ASTVisitor {
public:
    std::vector<ReferenceOccurrence> occurrences;

    void add(const SourceLocation& loc, int length, bool isDecl,
             const ResolverSourceLocation& declLoc, const std::string& declModule) {
        if (loc.Line > 0 && length > 0 && declLoc.Line > 0) {
            occurrences.push_back({loc, length, isDecl, declLoc, declModule});
        }
    }

    void visitVariable(VariableExprAST &Node) override {
        if (auto sym = Node.getResolvedSymbol()) {
            add(Node.getLoc(), static_cast<int>(Node.getName().length()), false, sym->DeclLoc, "");
        }
    }

    void visitVarDecl(VarDeclExprAST &Node) override {
        if (auto sym = Node.getResolvedSymbol()) {
            add(Node.getNameLoc(), static_cast<int>(Node.getName().length()), true, sym->DeclLoc, "");
        }
        if (Node.getInitializer()) const_cast<ExprAST*>(Node.getInitializer())->accept(*this);
    }

    void visitFuncDef(FuncDefExprAST &Node) override {
        if (auto target = Node.getResolvedDef()) {
            add(Node.getNameLoc(), bareNameLength(Node.getName()), true, target->DeclLoc, target->SourceModule);
        }
        if (Node.getBodyMut()) const_cast<ExprAST*>(Node.getBodyMut())->accept(*this);
    }

    void visitFuncCall(FuncCallExprAST &Node) override {
        if (auto target = Node.getResolvedCallTarget()) {
            SourceLocation loc = Node.getLoc();
            int len = bareNameLength(target->BaseName);
            // `Module::func(...)`: same special-case hover()/getDeclarationLoc()
            // already need -- the resolved target lives on the FuncCallExprAST,
            // not the ScopeAccessExprAST callee, but the visible NAME span is
            // the callee's right-hand side.
            if (auto scopeCallee = dynamic_cast<const ScopeAccessExprAST*>(Node.getCallee())) {
                loc = scopeCallee->getRightLoc();
                len = static_cast<int>(scopeCallee->getRightName().length());
            } else if (auto varCallee = dynamic_cast<const VariableExprAST*>(Node.getCallee())) {
                loc = varCallee->getLoc();
                len = static_cast<int>(varCallee->getName().length());
            }
            add(loc, len, false, target->DeclLoc, target->SourceModule);
        } else if (Node.getCallee()) {
            const_cast<ExprAST*>(Node.getCallee())->accept(*this);
        }
        for (const auto& arg : Node.getArgs()) {
            if (arg) const_cast<ExprAST*>(arg.get())->accept(*this);
        }
    }

    void visitMethodCall(MethodCallExprAST &Node) override {
        if (Node.getBase()) const_cast<ExprAST*>(Node.getBase())->accept(*this);
        if (auto target = Node.getResolvedCallTarget()) {
            add(Node.getNameLoc(), static_cast<int>(Node.getMethod().length()), false,
                target->DeclLoc, target->SourceModule);
        }
        for (const auto& arg : Node.getArgs()) {
            if (arg) const_cast<ExprAST*>(arg.get())->accept(*this);
        }
    }

    void visitBlock(BlockExprAST &Node) override {
        for (const auto& expr : Node.getExpressions())
            if (expr) const_cast<ExprAST*>(expr.get())->accept(*this);
    }
    void visitIf(IfExprAST &Node) override {
        for (const auto& branch : Node.getBranches()) {
            if (branch.Cond) const_cast<ExprAST*>(branch.Cond.get())->accept(*this);
            if (branch.Body) const_cast<ExprAST*>(branch.Body.get())->accept(*this);
        }
        if (Node.getElseBody()) const_cast<ExprAST*>(Node.getElseBody())->accept(*this);
    }
    void visitWhile(WhileExprAST &Node) override {
        if (Node.getCond()) const_cast<ExprAST*>(Node.getCond())->accept(*this);
        if (Node.getBody()) const_cast<ExprAST*>(Node.getBody())->accept(*this);
    }
    void visitFor(ForExprAST &Node) override {
        if (Node.getStart()) const_cast<ExprAST*>(Node.getStart())->accept(*this);
        if (Node.getEnd()) const_cast<ExprAST*>(Node.getEnd())->accept(*this);
        if (Node.getStep()) const_cast<ExprAST*>(Node.getStep())->accept(*this);
        if (Node.getBody()) const_cast<ExprAST*>(Node.getBody())->accept(*this);
    }
    void visitBinary(BinaryExprAST &Node) override {
        if (Node.getLHS()) const_cast<ExprAST*>(Node.getLHS())->accept(*this);
        if (Node.getRHS()) const_cast<ExprAST*>(Node.getRHS())->accept(*this);
    }
    void visitAssignment(AssignmentExprAST &Node) override {
        if (Node.getLHS()) const_cast<ExprAST*>(Node.getLHS())->accept(*this);
        if (Node.getRHS()) const_cast<ExprAST*>(Node.getRHS())->accept(*this);
    }
    void visitCompoundAssignment(CompoundAssignmentExprAST &Node) override {
        if (Node.getLHS()) const_cast<ExprAST*>(Node.getLHS())->accept(*this);
        if (Node.getRHS()) const_cast<ExprAST*>(Node.getRHS())->accept(*this);
    }
    void visitUnary(UnaryExprAST &Node) override {
        if (Node.getOperand()) const_cast<ExprAST*>(Node.getOperand())->accept(*this);
    }
    void visitAddressOf(AddressOfExprAST &Node) override {
        if (Node.getOperand()) const_cast<ExprAST*>(Node.getOperand())->accept(*this);
    }
    void visitCast(CastExprAST &Node) override {
        if (Node.getOperand()) const_cast<ExprAST*>(Node.getOperand())->accept(*this);
    }
    void visitPostfixExpr(PostfixExprAST &Node) override {
        if (Node.getOperand()) const_cast<ExprAST*>(Node.getOperand())->accept(*this);
    }
    void visitReturn(ReturnExprAST &Node) override {
        if (Node.getVal()) const_cast<ExprAST*>(Node.getVal())->accept(*this);
    }
    void visitCompare(CompareExprAST &Node) override {
        if (Node.getSubject()) const_cast<ExprAST*>(Node.getSubject())->accept(*this);
        for (const auto& arm : Node.getArms()) {
            for (const auto& val : arm.Values)
                if (val) const_cast<ExprAST*>(val.get())->accept(*this);
            if (arm.Body) const_cast<ExprAST*>(arm.Body.get())->accept(*this);
        }
        if (Node.getOtherBody()) const_cast<ExprAST*>(Node.getOtherBody())->accept(*this);
    }
    void visitFieldAccess(FieldAccessExprAST &Node) override {
        if (Node.getBase()) const_cast<ExprAST*>(Node.getBase())->accept(*this);
    }
    void visitArrayDecl(ArrayDeclExprAST &Node) override {
        for (const auto& elem : Node.getInitList())
            if (elem) const_cast<ExprAST*>(elem.get())->accept(*this);
    }
    void visitArrayLiteral(ArrayLiteralExprAST &Node) override {
        for (const auto& elem : Node.getElements())
            if (elem) const_cast<ExprAST*>(elem.get())->accept(*this);
    }
    void visitArrayAccess(ArrayAccessExprAST &Node) override {
        if (Node.getBase()) const_cast<ExprAST*>(Node.getBase())->accept(*this);
        if (Node.getIndex()) const_cast<ExprAST*>(Node.getIndex())->accept(*this);
    }
};
} // namespace

std::vector<ReferenceOccurrence> SemanticQuery::findAllReferenceOccurrences(const DocumentAnalysis& analysis) {
    ReferenceVisitor visitor;
    for (size_t i = analysis.preludeImportCount(); i < analysis.asts().size(); ++i) {
        if (analysis.asts()[i]) const_cast<ExprAST*>(analysis.asts()[i].get())->accept(visitor);
    }
    return std::move(visitor.occurrences);
}

std::vector<DocumentSymbolEntry> SemanticQuery::getDocumentSymbols(const DocumentAnalysis& analysis) {
    std::vector<DocumentSymbolEntry> symbols;
    // Top-level entries only -- analysis.asts() IS the file's flat top-level
    // declaration list, so no AST walk is needed, just a dynamic_cast per
    // entry (skips anything nested inside a function body by construction).
    for (size_t i = analysis.preludeImportCount(); i < analysis.asts().size(); ++i) {
        const ExprAST* node = analysis.asts()[i].get();
        if (!node) continue;
        if (auto structDecl = dynamic_cast<const StructDeclExprAST*>(node)) {
            symbols.push_back({structDecl->getName(), 23 /* Struct */,
                               structDecl->getNameLoc(), static_cast<int>(structDecl->getName().length())});
        } else if (auto enumDecl = dynamic_cast<const EnumDeclExprAST*>(node)) {
            // getLoc() is the `enum` keyword (see TypeDeclIndexVisitor's
            // identical note) -- EnumDeclExprAST has no separate NameLoc.
            symbols.push_back({enumDecl->getName(), 10 /* Enum */, enumDecl->getLoc(), 1});
        } else if (auto fnDef = dynamic_cast<const FuncDefExprAST*>(node)) {
            symbols.push_back({fnDef->getName(), 12 /* Function */,
                               fnDef->getNameLoc(), bareNameLength(fnDef->getName())});
        }
    }
    return symbols;
}

// ─── Completion ──────────────────────────────────────────────────────────

namespace {

// Walks the whole document once per completion request, collecting:
//  - every VarDeclExprAST's name -> resolved type. Flat, not scope-depth-
//    aware: the Resolver's own scope stack is already unwound by the time
//    resolveAll() returns, so precise "visible at this exact point" lookup
//    isn't available post-hoc. Over-suggesting an out-of-scope variable is a
//    far smaller problem for a completion list than missing a real one.
//  - every StructDeclExprAST by name (including ones declared inside
//    imported modules, via visitModuleImport's recursion), so a variable's
//    NamedType can be turned into its field/method list.
class DocumentIndexVisitor : public ASTVisitor {
public:
    std::unordered_map<std::string, TurfTypePtr> VariableTypes;
    std::unordered_map<std::string, const StructDeclExprAST*> StructDecls;
    // Declaration line (1-indexed compiler convention) per variable name --
    // doc 05 G3's scope-distance ranking approximation. Not real scope
    // depth (this visitor is flat, see the class comment above), just "how
    // many lines away from the cursor was this declared" -- cheap, and a
    // reasonable proxy: a variable declared 2 lines up is far more likely
    // to be what the user means than one declared 200 lines up in an
    // unrelated function.
    std::unordered_map<std::string, int> VariableDeclLine;

    void visitVarDecl(VarDeclExprAST &Node) override {
        if (auto sym = Node.getResolvedSymbol()) {
            VariableTypes[Node.getName()] = sym->Type;
            VariableDeclLine[Node.getName()] = Node.getNameLoc().Line;
        }
        if (Node.getInitializer()) const_cast<ExprAST*>(Node.getInitializer())->accept(*this);
    }
    void visitStructDecl(StructDeclExprAST &Node) override {
        StructDecls[Node.getName()] = &Node;
    }
    void visitModuleImport(ModuleImportExprAST &Node) override {
        if (Node.getBody()) const_cast<ExprAST*>(Node.getBody())->accept(*this);
    }
    void visitBlock(BlockExprAST &Node) override {
        for (const auto& e : Node.getExpressions())
            if (e) const_cast<ExprAST*>(e.get())->accept(*this);
    }
    void visitIf(IfExprAST &Node) override {
        for (const auto& b : Node.getBranches()) {
            if (b.Cond) const_cast<ExprAST*>(b.Cond.get())->accept(*this);
            if (b.Body) const_cast<ExprAST*>(b.Body.get())->accept(*this);
        }
        if (Node.getElseBody()) const_cast<ExprAST*>(Node.getElseBody())->accept(*this);
    }
    void visitWhile(WhileExprAST &Node) override {
        if (Node.getBody()) const_cast<ExprAST*>(Node.getBody())->accept(*this);
    }
    void visitFor(ForExprAST &Node) override {
        if (Node.getBody()) const_cast<ExprAST*>(Node.getBody())->accept(*this);
    }
    void visitFuncDef(FuncDefExprAST &Node) override {
        if (Node.getBodyMut()) const_cast<ExprAST*>(Node.getBodyMut())->accept(*this);
    }
};

struct TriggerContext {
    enum class Kind { ScopeAccess, MemberAccess, Bare, Import };
    Kind kind;
    std::string base;   // identifier before '::' or '.'; empty for Bare/Import
    std::string prefix; // partial identifier already typed after the trigger
};

// Purely textual: scans the line up to the cursor backward over identifier
// characters to find what's already been typed, then checks what precedes
// it. No parser/AST involvement -- this has to work even when the token
// under the cursor is mid-typing and wouldn't parse at all.
TriggerContext classifyTrigger(const std::string& text) {
    int pos = static_cast<int>(text.size());
    int prefixStart = pos;
    while (prefixStart > 0 && isIdentChar(text[prefixStart - 1])) prefixStart--;
    std::string prefix = text.substr(prefixStart, pos - prefixStart);

    // `import <prefix>` -- bare module-name completion (doc 05 G2). Checked
    // before the '::'/'.' checks below because "import net" has no trailing
    // '::' or '.' of its own, so without this it would fall through to Bare
    // (keywords/locals/current-file functions) -- wrong, there's no local
    // variable or keyword spelled like a stdlib module name. turf-lang has
    // no `from X import Y` form (confirmed by reading ParserModule.cpp --
    // the only `from` in the grammar is `extern from "lib" fn ...`), so
    // that's the only import shape to detect.
    if (prefixStart > 0) {
        size_t textEnd = text.find_last_not_of(" \t", prefixStart - 1);
        if (textEnd != std::string::npos && textEnd >= 5 &&
            text.compare(textEnd - 5, 6, "import") == 0 &&
            (textEnd == 5 || !isIdentChar(text[textEnd - 6]))) {
            return {TriggerContext::Kind::Import, "", prefix};
        }
    }

    if (prefixStart >= 2 && text[prefixStart - 1] == ':' && text[prefixStart - 2] == ':') {
        int baseEnd = prefixStart - 2;
        int baseStart = baseEnd;
        while (baseStart > 0 && isIdentChar(text[baseStart - 1])) baseStart--;
        return {TriggerContext::Kind::ScopeAccess, text.substr(baseStart, baseEnd - baseStart), prefix};
    }
    if (prefixStart >= 1 && text[prefixStart - 1] == '.') {
        int baseEnd = prefixStart - 1;
        int baseStart = baseEnd;
        while (baseStart > 0 && isIdentChar(text[baseStart - 1])) baseStart--;
        return {TriggerContext::Kind::MemberAccess, text.substr(baseStart, baseEnd - baseStart), prefix};
    }
    return {TriggerContext::Kind::Bare, "", prefix};
}

// Looks up the single overload registered under `fullName` (e.g.
// "Point.distance") in either the current file (ownerModule empty) or the
// named module. Returns nullptr if nothing is registered under that exact
// name in that context.
const ResolverFunctionSymbol* firstOverload(const ResolverSymbolTable& symTable,
                                            const std::string& fullName,
                                            const std::string& ownerModule) {
    if (ownerModule.empty()) {
        auto overloads = symTable.lookupCurrentFileFunctions(fullName);
        return overloads.empty() ? nullptr : overloads.front();
    }
    for (const auto& match : symTable.findInModules(fullName)) {
        if (match.ModuleName == ownerModule) return match.Symbol;
    }
    return nullptr;
}

// A struct method has `self` as its first parameter (2.8.1 in the module-
// system doc); a true static method never does. This is what distinguishes
// `StructName::method` (static) completions from `instance.method`
// (instance) ones -- both are stored under the identical flat
// "StructName.method" name, so the split has to come from the symbol data.
bool isInstanceMethod(const ResolverFunctionSymbol* sym) {
    return sym && !sym->Params.empty() && sym->Params[0].Name == "self";
}

std::string buildCallSnippet(const std::string& label, const ResolverFunctionSymbol& sym, bool skipSelf) {
    std::string snippet = label + "(";
    int slot = 1;
    bool first = true;
    for (size_t i = 0; i < sym.Params.size(); ++i) {
        if (skipSelf && i == 0 && sym.Params[i].Name == "self") continue;
        if (!first) snippet += ", ";
        first = false;
        snippet += "${" + std::to_string(slot++) + ":" + sym.Params[i].Name + "}";
    }
    snippet += ")$0";
    return snippet;
}

// Every `<structName>.<method>` name registered in `ownerModule` (the main
// file if empty), matched back to its ResolverFunctionSymbol. Reuses the
// Resolver's existing "StructName.method" flat internal naming convention
// for struct methods (docs/architecture/modules_and_scope_resolution.md
// §2.8.1) rather than needing a dedicated turf-lang API for "list this
// struct's methods".
std::vector<std::pair<std::string, const ResolverFunctionSymbol*>>
structMethods(const ResolverSymbolTable& symTable, const std::string& structName, const std::string& ownerModule) {
    std::vector<std::pair<std::string, const ResolverFunctionSymbol*>> out;
    std::string prefix = structName + ".";
    std::vector<std::string> allNames = ownerModule.empty()
        ? symTable.getAllCurrentFileFunctionNames()
        : symTable.getModuleFunctionNames(ownerModule);
    for (const auto& full : allNames) {
        if (full.rfind(prefix, 0) != 0) continue;
        std::string method = full.substr(prefix.size());
        out.emplace_back(method, firstOverload(symTable, full, ownerModule));
    }
    return out;
}

// ── Keywords and snippets ───────────────────────────────────────────────────
//
// The reserved words come from the compiler (turf::keywords(), the lexer's own
// table), so a word the language adds is offered without anyone remembering to
// add it here. What this file decides is where each one makes sense.

// Declarations that only exist at the top level of a file.
bool isTopLevelOnlyKeyword(const std::string& name) {
    static const std::set<std::string> names = {"fn", "import", "extern", "export", "static", "operator", "from"};
    return names.count(name) > 0;
}

bool isVisibilityKeyword(const std::string& name) { return name == "pub" || name == "public"; }

// Declarations that are valid both at the top level and inside a function
// (a local struct, a local variable).
bool isDeclarationAnywhere(const std::string& name) {
    static const std::set<std::string> names = {"struct", "enum", "machine", "mut", "mutable",
                                                "immut", "immutable", "unsafe"};
    return names.count(name) > 0;
}

bool keywordFits(const turf::Keyword& kw, CompletionRegion region) {
    switch (region) {
    case CompletionRegion::TypeBody:
        // A field's type, or `pub` in front of it.
        return kw.Kind == turf::KeywordKind::Type || isVisibilityKeyword(kw.Name);
    case CompletionRegion::TopLevel:
        return kw.Kind == turf::KeywordKind::Keyword &&
               (isTopLevelOnlyKeyword(kw.Name) || isVisibilityKeyword(kw.Name) || isDeclarationAnywhere(kw.Name));
    case CompletionRegion::Statement:
        if (kw.Kind != turf::KeywordKind::Keyword) return true; // types and literals
        return !isTopLevelOnlyKeyword(kw.Name) && !isVisibilityKeyword(kw.Name);
    }
    return false;
}

// What accepting the keyword inserts, when more than the word helps. Checked
// against the compiler by scripts/smoke.py (each must parse once its
// placeholders are filled in).
const char* keywordSnippet(const std::string& name, CompletionRegion region) {
    static const std::map<std::string, const char*> topLevel = {
        {"fn", "fn ${1:name}(${2}): ${3:int} {\n    $0\n}"},
        {"struct", "struct ${1:Name} {\n    ${2:field}: ${3:int}\n}"},
        {"enum", "enum ${1:Name} { ${2:First}, ${3:Second} }"},
        {"import", "import ${1:io};"},
        {"extern", "extern fn ${1:name}(${2}): ${3:int};"},
    };
    static const std::map<std::string, const char*> statement = {
        {"if", "if ${1:condition} {\n    $0\n}"},
        {"elseif", "elseif ${1:condition} {\n    $0\n}"},
        {"else", "else {\n    $0\n}"},
        {"while", "while ${1:condition} {\n    $0\n}"},
        {"do", "do {\n    $0\n} while ${1:condition};"},
        {"for", "for ${1:i} in ${2:0}..${3:10} step ${1:i} += 1 {\n    $0\n}"},
        {"compare", "compare ${1:value} {\n    matches ${2:1}:\n        $0\n    done\n    otherwise:\n        ${3:// default}\n}"},
        {"mut", "mut ${1:name}: ${2:int} = ${3:0};"},
        {"immut", "immut ${1:name}: ${2:int} = ${3:0};"},
        {"defer", "defer ${1:cleanup()};"},
        {"return", "return ${1:0};"},
    };
    const auto& table = region == CompletionRegion::TopLevel ? topLevel : statement;
    if (region == CompletionRegion::TypeBody) return nullptr;
    auto it = table.find(name);
    return it == table.end() ? nullptr : it->second;
}

void appendKeywordItems(std::vector<CompletionItem>& items, CompletionRegion region) {
    for (const auto& kw : turf::keywords()) {
        if (!keywordFits(kw, region)) continue;
        CompletionItem item;
        item.label = kw.Name;
        switch (kw.Kind) {
        case turf::KeywordKind::Keyword: item.kind = 14; item.detail = "keyword"; break;
        case turf::KeywordKind::Type:    item.kind = 7;  item.detail = "type"; break;
        case turf::KeywordKind::Literal: item.kind = 21; item.detail = "literal"; break;
        }
        if (const char* snippet = keywordSnippet(kw.Name, region)) item.insertSnippet = snippet;
        item.sortText = "2" + kw.Name;
        items.push_back(std::move(item));
    }
}

// Snippets that are not a keyword of their own.
void appendSnippetItems(std::vector<CompletionItem>& items, CompletionRegion region) {
    struct Snippet { const char* label; const char* detail; const char* body; };
    static const Snippet topLevel[] = {
        {"main", "program entry point", "fn main(): int {\n    $0\n    return 0;\n}"},
        {"method", "method of a struct", "fn ${1:Type}.${2:name}(${3}): ${4:void} {\n    $0\n}"},
    };
    static const Snippet statement[] = {
        {"ifelse", "if / else", "if ${1:condition} {\n    $2\n} else {\n    $0\n}"},
        {"foreach", "for each element", "for ${1:item} in ${2:items} {\n    $0\n}"},
    };
    auto add = [&](const Snippet& s) {
        CompletionItem item;
        item.label = s.label;
        item.kind = 15; // Snippet
        item.detail = s.detail;
        item.insertSnippet = s.body;
        item.sortText = std::string("3") + s.label;
        items.push_back(std::move(item));
    };
    if (region == CompletionRegion::TopLevel) for (const auto& s : topLevel) add(s);
    if (region == CompletionRegion::Statement) for (const auto& s : statement) add(s);
}

} // namespace

// ── Signature help ──────────────────────────────────────────────────────────

namespace {

struct CallSite {
    std::string callee;   // as written: `add`, `io::printline`, `p.sum`, `Point::new`
    int activeParam = 0;  // how many commas of this call's arguments precede the cursor
};

// The innermost call whose argument list the cursor is in, found by scanning
// the text from the start -- ignoring strings, character literals and `//`
// comments -- and remembering each unclosed `(`, `[` or `{` with the commas
// seen at its own level. A `(` that follows an identifier is a call; one that
// does not (`(a + b)`) is grouping and is skipped.
std::optional<CallSite> findCallSite(const std::string& text, size_t byteOffset) {
    struct Open { char ch; size_t at; int commas; };
    std::vector<Open> stack;
    const size_t end = std::min(byteOffset, text.size());
    for (size_t i = 0; i < end; ++i) {
        const char ch = text[i];
        if (ch == '/' && i + 1 < end && text[i + 1] == '/') {
            while (i < end && text[i] != '\n') ++i;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            ++i;
            while (i < end && text[i] != ch) { if (text[i] == '\\') ++i; ++i; }
            continue;
        }
        switch (ch) {
        case '(': case '[': case '{': stack.push_back({ch, i, 0}); break;
        case ')': case ']': case '}': if (!stack.empty()) stack.pop_back(); break;
        case ',': if (!stack.empty()) ++stack.back().commas; break;
        case ';':
            // A statement ended, so a `(` or `[` still open in this block was
            // never closed (half-typed text); it is not around the cursor.
            while (!stack.empty() && stack.back().ch != '{') stack.pop_back();
            break;
        default: break;
        }
    }
    for (size_t k = stack.size(); k-- > 0;) {
        if (stack[k].ch == '{') break;       // a block is not an argument list
        if (stack[k].ch != '(') continue;
        size_t stop = stack[k].at;
        while (stop > 0 && text[stop - 1] == ' ') --stop;
        size_t start = stop;
        while (start > 0 && (isIdentChar(text[start - 1]) || text[start - 1] == ':' || text[start - 1] == '.')) --start;
        if (start == stop) continue;         // `(a + b)`: grouping, look further out
        return CallSite{text.substr(start, stop - start), stack[k].commas};
    }
    return std::nullopt;
}

// "Type name" for each parameter, as signature() spells them; `self` is
// dropped for a call written `value.method(...)`, where it is the receiver.
SemanticQuery::SignatureInfo describeSignature(const ResolverFunctionSymbol& sym, bool dropSelf) {
    SemanticQuery::SignatureInfo info;
    std::string name = sym.BaseName;
    if (auto dot = name.rfind('.'); dot != std::string::npos) name = name.substr(dot + 1);
    std::string label = name + "(";
    bool first = true;
    for (size_t i = 0; i < sym.Params.size(); ++i) {
        if (dropSelf && i == 0 && sym.Params[i].Name == "self") continue;
        std::string p = (sym.Params[i].Type ? sym.Params[i].Type->getName() : std::string("?"));
        if (!sym.Params[i].Name.empty()) p += " " + sym.Params[i].Name;
        if (!first) label += ", ";
        first = false;
        label += p;
        info.params.push_back(std::move(p));
    }
    label += ") -> " + (sym.ReturnType ? sym.ReturnType->getName() : std::string("void"));
    info.label = std::move(label);
    return info;
}

// Every overload registered under `name` in `ownerModule` (the current file if
// empty), restricted to what another module may call.
std::vector<const ResolverFunctionSymbol*> overloadsOf(const ResolverSymbolTable& symTable, const std::string& name,
                                                       const std::string& ownerModule) {
    std::vector<const ResolverFunctionSymbol*> out;
    if (ownerModule.empty()) {
        out = symTable.lookupCurrentFileFunctions(name);
    } else {
        for (const auto& match : symTable.findInModules(name))
            if (match.ModuleName == ownerModule && match.Symbol && match.Symbol->IsPublic) out.push_back(match.Symbol);
    }
    return out;
}

} // namespace

namespace {

struct ResolvedCallee {
    std::vector<const ResolverFunctionSymbol*> overloads;
    bool dropSelf = false; // `value.method(...)`: `self` is the receiver, not an argument
};

// What a callee written as `add`, `io::printline`, `Point::new` or `p.scaled`
// refers to, against an analysis. `variableTypes` is the document's variables'
// types, built once by the caller when it resolves many callees.
ResolvedCallee resolveCallee(const DocumentAnalysis& analysis, const std::string& callee,
                             const std::map<std::string, TurfTypePtr>& variableTypes) {
    const ResolverSymbolTable& symTable = analysis.symbols();
    ResolvedCallee out;
    if (auto sep = callee.rfind("::"); sep != std::string::npos) {
        const std::string base = callee.substr(0, sep), name = callee.substr(sep + 2);
        if (symTable.hasModule(base)) {
            out.overloads = overloadsOf(symTable, name, base);
        } else if (symTable.isKnownStruct(base)) {
            out.overloads = overloadsOf(symTable, base + "." + name, symTable.getTypeOwningModule(base));
        }
    } else if (auto dot = callee.rfind('.'); dot != std::string::npos) {
        const std::string receiver = callee.substr(0, dot), name = callee.substr(dot + 1);
        if (auto it = variableTypes.find(receiver); it != variableTypes.end() && it->second) {
            const std::string structName = it->second->getName();
            out.overloads = overloadsOf(symTable, structName + "." + name, symTable.getTypeOwningModule(structName));
            out.dropSelf = true;
        }
    } else {
        out.overloads = overloadsOf(symTable, callee, "");
    }
    return out;
}

std::map<std::string, TurfTypePtr> variableTypesOf(const DocumentAnalysis& analysis) {
    DocumentIndexVisitor indexer;
    for (const auto& ast : analysis.asts())
        if (ast) const_cast<ExprAST*>(ast.get())->accept(indexer);
    std::map<std::string, TurfTypePtr> out;
    for (const auto& entry : indexer.VariableTypes) out[entry.first] = entry.second;
    return out;
}

} // namespace

std::optional<SemanticQuery::SignatureHelp> SemanticQuery::getSignatureHelp(const DocumentAnalysis& analysis,
                                                                           const std::string& text,
                                                                           size_t byteOffset) {
    auto site = findCallSite(text, byteOffset);
    if (!site) return std::nullopt;

    ResolvedCallee callee = resolveCallee(analysis, site->callee, variableTypesOf(analysis));
    const auto& overloads = callee.overloads;
    const bool dropSelf = callee.dropSelf;
    if (overloads.empty()) return std::nullopt;

    SignatureHelp help;
    help.activeParameter = site->activeParam;
    for (const auto* overload : overloads) help.signatures.push_back(describeSignature(*overload, dropSelf));
    // The first overload with room for the argument being typed.
    for (size_t i = 0; i < help.signatures.size(); ++i) {
        if (static_cast<int>(help.signatures[i].params.size()) > site->activeParam) {
            help.activeSignature = static_cast<int>(i);
            break;
        }
    }
    return help;
}

std::vector<SemanticQuery::InlayHintInfo> SemanticQuery::getInlayHints(const DocumentAnalysis& analysis,
                                                                       const std::string& text,
                                                                       size_t startByte, size_t endByte) {
    std::vector<InlayHintInfo> hints;
    static const std::set<std::string> reserved = [] {
        std::set<std::string> s;
        for (const auto& kw : turf::keywords()) s.insert(kw.Name);
        return s;
    }();
    const auto variableTypes = variableTypesOf(analysis);
    const size_t limit = std::min(endByte, text.size());

    auto previousWord = [&](size_t at) { // the word before `at`, skipping blanks
        while (at > 0 && (text[at - 1] == ' ' || text[at - 1] == '\t')) --at;
        size_t from = at;
        while (from > 0 && isIdentChar(text[from - 1])) --from;
        return text.substr(from, at - from);
    };

    for (size_t i = 0; i < limit; ++i) {
        const char ch = text[i];
        if (ch == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < limit && text[i] != '\n') ++i;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            ++i;
            while (i < text.size() && text[i] != ch && text[i] != '\n') { if (text[i] == '\\') ++i; ++i; }
            continue;
        }
        if (ch != '(') continue;

        size_t stop = i;
        while (stop > 0 && text[stop - 1] == ' ') --stop;
        size_t start = stop;
        while (start > 0 && (isIdentChar(text[start - 1]) || text[start - 1] == ':' || text[start - 1] == '.')) --start;
        if (start == stop) continue; // `(a + b)`: grouping
        const std::string callee = text.substr(start, stop - start);
        if (callee.find("::") == std::string::npos && callee.find('.') == std::string::npos && reserved.count(callee))
            continue; // `if (`, `while (`, a cast
        if (previousWord(start) == "fn") continue; // a declaration, not a call

        // The arguments: where each starts and ends, at this call's own level.
        std::vector<std::pair<size_t, size_t>> args;
        {
            int depth = 0;
            size_t argStart = std::string::npos;
            size_t j = i + 1;
            for (; j < text.size(); ++j) {
                const char c = text[j];
                if (c == '"' || c == '\'') {
                    if (argStart == std::string::npos) argStart = j;
                    ++j;
                    while (j < text.size() && text[j] != c && text[j] != '\n') { if (text[j] == '\\') ++j; ++j; }
                    continue;
                }
                if (c == '(' || c == '[' || c == '{') { if (argStart == std::string::npos) argStart = j; ++depth; continue; }
                if (c == ')' || c == ']' || c == '}') {
                    if (depth == 0) break; // this call's closing parenthesis
                    --depth;
                    continue;
                }
                if (c == ',' && depth == 0) {
                    if (argStart != std::string::npos) args.emplace_back(argStart, j);
                    argStart = std::string::npos;
                    continue;
                }
                if (c == ';') { j = text.size(); break; } // a statement ended inside the parentheses: the call was never closed
                if (argStart == std::string::npos && c != ' ' && c != '\t' && c != '\n' && c != '\r') argStart = j;
            }
            if (j >= text.size()) continue; // an unclosed call: nothing reliable to say
            if (argStart != std::string::npos) args.emplace_back(argStart, j);
        }
        if (args.empty()) continue;

        const ResolvedCallee resolved = resolveCallee(analysis, callee, variableTypes);
        for (const auto* sym : resolved.overloads) {
            if (!sym || sym->IsVarArg) continue;
            std::vector<std::string> names;
            for (size_t p = 0; p < sym->Params.size(); ++p) {
                if (resolved.dropSelf && p == 0 && sym->Params[p].Name == "self") continue;
                names.push_back(sym->Params[p].Name);
            }
            if (names.size() != args.size()) continue;
            for (size_t k = 0; k < args.size(); ++k) {
                if (names[k].empty() || args[k].first < startByte) continue;
                size_t e = args[k].second;
                while (e > args[k].first && (text[e - 1] == ' ' || text[e - 1] == '\t' || text[e - 1] == '\n' || text[e - 1] == '\r')) --e;
                if (text.compare(args[k].first, e - args[k].first, names[k]) == 0) continue; // `x` for parameter `x`
                hints.push_back({args[k].first, names[k] + ":"});
            }
            break; // the first overload that fits
        }
    }
    return hints;
}

SemanticQuery::ImportInsertion SemanticQuery::importInsertion(const std::string& text, const std::string& module) {
    return importStatementInsertion(text, "import " + module + ";");
}

SemanticQuery::ImportInsertion SemanticQuery::importStatementInsertion(const std::string& text,
                                                                       const std::string& statement) {
    std::vector<std::string> lines;
    {
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(std::move(line));
        }
    }
    const bool endsWithNewline = !text.empty() && text.back() == '\n';

    auto startsWith = [](const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; };
    int lastImport = -1;
    for (size_t i = 0; i < lines.size(); ++i)
        if (startsWith(lines[i], "import ")) lastImport = static_cast<int>(i);

    ImportInsertion at;
    if (lastImport >= 0) {
        const int next = lastImport + 1;
        if (next < static_cast<int>(lines.size()) || endsWithNewline) {
            at.line = next;
            at.text = statement + "\n";
        } else { // the file ends right after its last import, with no newline
            at.line = lastImport;
            at.character = static_cast<int>(lines[lastImport].size());
            at.text = "\n" + statement;
        }
        return at;
    }

    // No imports yet: after the header, with a blank line to set them off.
    int header = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        size_t first = lines[i].find_first_not_of(" \t");
        const std::string trimmed = first == std::string::npos ? "" : lines[i].substr(first);
        if (trimmed.empty() || startsWith(trimmed, "//") || trimmed == "unsafe module;") header = static_cast<int>(i) + 1;
        else break;
    }
    at.line = header;
    at.text = statement + "\n\n";
    return at;
}

CompletionRegion SemanticQuery::regionAt(const std::string& text, size_t byteOffset) {
    std::vector<char> blocks; // 'T' a struct/enum body, 'F' anything else (a function body, a nested block)
    std::string header;       // the text since the last `;`, `{` or `}`: what the next `{` opens
    const size_t end = std::min(byteOffset, text.size());
    auto isTypeHeader = [](const std::string& h) {
        std::istringstream words(h);
        std::string w;
        while (words >> w) {
            if (w == "pub" || w == "public" || w == "export") continue;
            return w == "struct" || w == "enum" || w == "machine";
        }
        return false;
    };
    for (size_t i = 0; i < end; ++i) {
        const char ch = text[i];
        if (ch == '/' && i + 1 < end && text[i + 1] == '/') { // line comment
            while (i < end && text[i] != '\n') ++i;
            continue;
        }
        if (ch == '"' || ch == '\'') { // a string or character literal, with its escapes
            ++i;
            while (i < end && text[i] != ch) { if (text[i] == '\\') ++i; ++i; }
            header += ' ';
            continue;
        }
        if (ch == '{') { blocks.push_back(isTypeHeader(header) ? 'T' : 'F'); header.clear(); continue; }
        if (ch == '}') { if (!blocks.empty()) blocks.pop_back(); header.clear(); continue; }
        if (ch == ';') { header.clear(); continue; }
        header += ch;
    }
    if (blocks.empty()) return CompletionRegion::TopLevel;
    return blocks.back() == 'T' ? CompletionRegion::TypeBody : CompletionRegion::Statement;
}

std::vector<CompletionItem> SemanticQuery::getCompletions(const DocumentAnalysis& analysis,
                                                           const std::string& lineTextBeforeCursor,
                                                           const CompletionPlace& where) {
    std::vector<CompletionItem> items;

    TriggerContext ctx = classifyTrigger(lineTextBeforeCursor);

    // `import <prefix>` -- module-name completion (doc 05 G2). Answered from
    // the analysis's own list of what an import here could name, which needs
    // no resolved AST -- an in-progress `import ` line (no `;` yet) is exactly
    // the kind of buffer the patched-reanalysis path may not fully resolve --
    // and which follows the context the analysis ran in: in a project, the
    // package's own modules and dependencies, not just the files next to this
    // one. (It is the analysis's, not a fresh filesystem lookup, because the
    // project's graph is only in force while the analysis runs.)
    if (ctx.kind == TriggerContext::Kind::Import) {
        for (const auto& name : analysis.result.importableModules()) {
            CompletionItem item;
            item.label = name;
            item.kind = 9; // Module
            item.detail = "module";
            item.insertSnippet = name + ";";
            items.push_back(std::move(item));
        }
        return items;
    }

    const ResolverSymbolTable& symTable = analysis.symbols();

    if (ctx.kind == TriggerContext::Kind::ScopeAccess) {
        const std::string& base = ctx.base;
        if (symTable.hasModule(base)) {
            const size_t firstMember = items.size();
            // Only what another module may call (LEC-002: declarations are
            // private unless `pub`); offering a private function would insert
            // a call the compiler rejects.
            for (const auto& name : symTable.getModulePublicFunctionNames(base)) {
                if (auto sym = firstOverload(symTable, name, base)) {
                    CompletionItem item;
                    item.label = name;
                    item.kind = 3; // Function
                    item.detail = sym->signature();
                    item.insertSnippet = buildCallSnippet(name, *sym, false);
                    item.resolveName = name;
                    item.resolveModule = base;
                    items.push_back(std::move(item));
                }
            }
            for (const auto& name : symTable.getModuleTypeNames(base)) {
                items.push_back({name, 22, "type in " + base, ""});
            }
            for (const auto& name : symTable.getModuleConstantNames(base)) {
                items.push_back({name, 21, "constant in " + base, ""});
            }
            // hasModule() means "loaded somewhere in the program", which is not
            // "this file may write `base::`": the prelude loads `list` (through
            // `strings`), and an import elsewhere loads whatever it names. A file
            // may spell `m::` only for a module it imports itself (compiler
            // LEC-008), so for any other loaded module accepting a member also
            // adds `import m;` -- inserting the bare call alone would be E145.
            if (!symTable.isModuleImportedBy(base, ""))
                for (size_t i = firstMember; i < items.size(); ++i)
                    items[i].autoImportModule = base;
        } else if (symTable.isEnumName(base)) {
            if (auto enumType = symTable.lookupEnumType(base)) {
                for (const auto& variant : enumType->getVariants()) {
                    items.push_back({variant, 20, base + "::" + variant, ""});
                }
                items.push_back({"COUNT", 21, "int", ""});
                items.push_back({"VALUES", 21, "int*", ""});
                items.push_back({"NAMES", 21, "string*", ""});
            }
        } else if (symTable.isKnownStruct(base)) {
            std::string owner = symTable.getTypeOwningModule(base);
            items.push_back({"new", 4, base + "::new(...)", "new(${1:args})$0"});
            items.push_back({"default", 4, base + "::default()", "default()$0"});
            for (const auto& entry : structMethods(symTable, base, owner)) {
                const std::string& method = entry.first;
                const ResolverFunctionSymbol* sym = entry.second;
                if (method == "new" || method == "default") continue;
                if (isInstanceMethod(sym)) continue; // static-only after '::'
                if (!owner.empty() && sym && !sym->IsPublic) continue; // another module's private method
                CompletionItem item;
                item.label = method;
                item.kind = 2; // Method (static)
                item.detail = sym ? sym->signature() : (base + "." + method + "()");
                item.insertSnippet = sym ? buildCallSnippet(method, *sym, false) : (method + "()$0");
                item.resolveName = base + "." + method; // structMethods()'s flat symbol-table key
                item.resolveModule = owner;
                items.push_back(std::move(item));
            }
        }
        return items;
    }

    if (ctx.kind == TriggerContext::Kind::MemberAccess) {
        DocumentIndexVisitor indexer;
        for (const auto& ast : analysis.asts())
            if (ast) const_cast<ExprAST*>(ast.get())->accept(indexer);

        auto varIt = indexer.VariableTypes.find(ctx.base);
        if (varIt == indexer.VariableTypes.end() || !varIt->second) return items;
        std::string structName = varIt->second->getName();

        auto declIt = indexer.StructDecls.find(structName);
        if (declIt == indexer.StructDecls.end()) return items; // not a struct instance

        for (const auto& field : declIt->second->getFields()) {
            CompletionItem item;
            item.label = field.Name;
            item.kind = 5; // Field
            item.detail = field.Type ? field.Type->getName() : "";
            // doc 05 G6 acceptance test: "p. lists its fields then instance
            // methods" -- true of the emission order below already, but a
            // client without an explicit sortText sorts the list
            // alphabetically itself, silently interleaving fields and
            // methods back together. "0" beats methods' "1" below.
            item.sortText = "0" + field.Name;
            items.push_back(std::move(item));
        }

        std::string owner = symTable.getTypeOwningModule(structName);
        for (const auto& entry : structMethods(symTable, structName, owner)) {
            const std::string& method = entry.first;
            const ResolverFunctionSymbol* sym = entry.second;
            if (method == "new" || method == "default") continue; // ConstructorCalledOnInstanceError
            if (!isInstanceMethod(sym)) continue; // instance-only after '.'
            if (!owner.empty() && !sym->IsPublic) continue; // another module's private method
            CompletionItem item;
            item.label = method;
            item.kind = 2; // Method
            item.detail = sym->signature();
            item.insertSnippet = buildCallSnippet(method, *sym, true);
            item.resolveName = structName + "." + method;
            item.resolveModule = owner;
            item.sortText = "1" + method;
            items.push_back(std::move(item));
        }
        return items;
    }

    // Bare word: what is usable unqualified at the cursor. Rank tiers (doc
    // 05 G6) so a real, specific symbol beats a generic keyword that merely
    // sorts earlier alphabetically: "0" locals, "1" this file's functions and
    // types and imported modules, "2" keywords and built-in types, "3"
    // snippets.
    const CompletionRegion region = where.region;
    appendKeywordItems(items, region);
    appendSnippetItems(items, region);
    if (region == CompletionRegion::Statement)
        items.push_back({"assert", 3, "Builtin", "assert(${1:condition}, \"${2:message}\")$0", "", "", "2assert"});

    // Variables and parameters in scope AT THE CURSOR, from the analysis
    // itself. (Not "every variable the resolver ever saw": that included the
    // locals of every stdlib function the program imports.) Types that this
    // document can name come further down.
    if (region == CompletionRegion::Statement) {
        for (const auto& sym : analysis.result.symbolsInScope(where.documentPath, where.line, where.col)) {
            if (sym.Kind != turf::SymbolKind::Variable && sym.Kind != turf::SymbolKind::Parameter) continue;
            CompletionItem item;
            item.label = sym.Name;
            item.kind = 6;
            item.detail = sym.Type ? sym.Type->getName() : "";
            // Scope-distance ranking (doc 05 G3): closer declarations first,
            // any declared AFTER the cursor (a real but less-likely-intended
            // match -- e.g. still mid-typing the enclosing function) pushed
            // behind every backward one instead of competing on raw distance.
            int declLine = sym.Declaration ? static_cast<int>(sym.Declaration->Line) : 0;
            int distance = 999999;
            if (where.line > 0 && declLine > 0) {
                distance = (declLine <= where.line) ? (where.line - declLine) : (100000 + (declLine - where.line));
            }
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%06d", distance);
            item.sortText = "0" + std::string(buf) + sym.Name;
            items.push_back(std::move(item));
        }
        // The modules this file can name: the ones it imports (that is how the
        // standard library is reached, `io::printline`) and the prelude's. NOT
        // every module the resolver loaded -- `list` and `sys` are only there
        // because `io` imports them, and are not this file's to use unless it
        // imports them too. A prelude module that is also a type's name
        // (`int`, `double`) is already offered as the type.
        std::set<std::string> modules;
        for (const auto& ast : analysis.asts())
            if (auto* import = dynamic_cast<const ModuleImportExprAST*>(ast.get()))
                modules.insert(spelledModule(import->getModuleName()));
        for (const auto& name : modules) {
            bool dup = false;
            for (const auto& it : items) if (it.label == name) { dup = true; break; }
            if (dup) continue;
            CompletionItem item;
            item.label = name;
            item.kind = 9; // Module
            item.detail = "module";
            item.sortText = "1" + name;
            items.push_back(std::move(item));
        }
        // Modules this file could import but has not: offered too, and
        // accepting one adds its `import` line (auto-import), so the user can
        // type `json` and get `import json;` at the top without leaving the
        // line they are on. They rank below everything already usable.
        for (const auto& name : analysis.result.importableModules()) {
            if (modules.count(name)) continue;
            bool dup = false;
            for (const auto& it : items) if (it.label == name) { dup = true; break; }
            if (dup) continue;
            CompletionItem item;
            item.label = name;
            item.kind = 9; // Module
            item.detail = "module -- adds `import " + name + ";`";
            item.sortText = "4" + name;
            item.autoImportModule = name;
            items.push_back(std::move(item));
        }
    }

    for (const auto& name : symTable.getAllCurrentFileFunctionNames()) {
        if (region != CompletionRegion::Statement) break; // a call, not a declaration or a field type
        if (name.find('.') != std::string::npos) continue; // struct methods, not bare-callable
        if (auto sym = firstOverload(symTable, name, "")) {
            CompletionItem item;
            item.label = name;
            item.kind = 3;
            item.detail = sym->signature();
            item.insertSnippet = buildCallSnippet(name, *sym, false);
            item.resolveName = name;
            // resolveModule left "" -- current-file function.
            item.sortText = "1" + name;
            items.push_back(std::move(item));
        }
    }
    // The prelude's bare-callable functions (`printline`, `abs`, `exit`, ...:
    // every public free function of io, math and sys, compiler FREEZE.md 6.25).
    // They need no import and no qualifier, so they are offered as plain usable
    // names -- but only where the compiler would reach them: a function THIS
    // file declares with the same name hides the prelude's (the prelude is the
    // last resort of a bare call), so offering both would show a name that
    // resolves to the other one.
    if (region == CompletionRegion::Statement) {
        const auto ownFunctions = symTable.getAllCurrentFileFunctionNames();
        const std::set<std::string> hidden(ownFunctions.begin(), ownFunctions.end());
        for (const auto& module : turf::preludeBareModules()) {
            for (const auto& name : symTable.getModulePublicFunctionNames(module)) {
                if (name.find('.') != std::string::npos) continue; // methods, not bare-callable
                if (hidden.count(name)) continue;
                bool dup = false;
                for (const auto& it : items) if (it.label == name && it.kind == 3) { dup = true; break; }
                if (dup) continue; // two prelude modules never share a name today; stay safe
                if (auto sym = firstOverload(symTable, name, module)) {
                    CompletionItem item;
                    item.label = name;
                    item.kind = 3;
                    item.detail = sym->signature();
                    item.insertSnippet = buildCallSnippet(name, *sym, false);
                    item.resolveName = name;
                    item.resolveModule = module;
                    item.sortText = "1" + name;
                    items.push_back(std::move(item));
                }
            }
        }
    }
    // Types this document can name unqualified. A name with a `::` in it is a
    // type of an imported module (`list::List`), reached through the module
    // (`list::`), not by its bare name: offering it here would insert text
    // that does not compile.
    if (region != CompletionRegion::TopLevel) {
        for (const auto& name : symTable.getAllStructNames())
            if (name.find("::") == std::string::npos) items.push_back({name, 22, "struct", "", "", "", "1" + name});
        for (const auto& name : symTable.getAllEnumNames())
            if (name.find("::") == std::string::npos) items.push_back({name, 22, "enum", "", "", "", "1" + name});
    }

    return items;
}

std::optional<SymbolLocation> SemanticQuery::getFunctionDeclLoc(const DocumentAnalysis& analysis,
                                                                 const std::string& name,
                                                                 const std::string& module) {
    if (auto sym = firstOverload(analysis.symbols(), name, module)) {
        return SymbolLocation{sym->DeclLoc, module, bareNameLength(sym->BaseName)};
    }
    return std::nullopt;
}

bool SemanticQuery::isImportSymbolTrigger(const std::string& text, std::string& outModuleName) {
    int pos = static_cast<int>(text.size());
    int prefixStart = pos;
    while (prefixStart > 0 && isIdentChar(text[prefixStart - 1])) prefixStart--;

    // Must be right after a `::` (mirrors classifyTrigger()'s own
    // ScopeAccess detection -- duplicated rather than shared because that
    // one lives inside an anonymous namespace scoped to getCompletions()'s
    // internals, and this needs to be callable standalone, before any
    // analysis has run at all).
    if (prefixStart < 2 || text[prefixStart - 1] != ':' || text[prefixStart - 2] != ':') return false;

    int baseEnd = prefixStart - 2;
    int baseStart = baseEnd;
    while (baseStart > 0 && isIdentChar(text[baseStart - 1])) baseStart--;
    if (baseStart == baseEnd) return false; // "::foo" with no module name at all

    if (baseStart == 0) return false; // nothing before the module name to be "import"
    size_t textEnd = text.find_last_not_of(" \t", baseStart - 1);
    if (textEnd == std::string::npos || textEnd < 5) return false;
    if (text.compare(textEnd - 5, 6, "import") != 0) return false;
    if (textEnd != 5 && isIdentChar(text[textEnd - 6])) return false; // "reimport net::" must not match

    outModuleName = text.substr(baseStart, baseEnd - baseStart);
    return true;
}

std::vector<CompletionItem> SemanticQuery::getModuleExportedSymbols(const DocumentAnalysis& moduleAnalysis) {
    std::vector<CompletionItem> items;
    const ResolverSymbolTable& symTable = moduleAnalysis.symbols();

    for (const auto& name : symTable.getAllCurrentFileFunctionNames()) {
        if (name.find('.') != std::string::npos) continue; // struct methods, not `mod::name`-callable directly
        if (auto sym = firstOverload(symTable, name, "")) {
            CompletionItem item;
            item.label = name;
            item.kind = 3; // Function
            item.detail = sym->signature();
            item.insertSnippet = buildCallSnippet(name, *sym, false);
            items.push_back(std::move(item));
        }
    }
    for (const auto& name : symTable.getAllStructNames()) items.push_back({name, 22, "struct", ""});
    for (const auto& name : symTable.getAllEnumNames()) items.push_back({name, 22, "enum", ""});
    for (const auto& name : symTable.getModuleConstantNames("")) items.push_back({name, 21, "constant", ""});
    return items;
}
