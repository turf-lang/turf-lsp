#include "SemanticQuery.hpp"
#include "ASTVisitor.h"
#include "Parser.h" // ListAvailableModules() -- import completion (doc 05 G2)
#include "Resolver.h"
#include <cctype>
#include <iostream>
#include <unordered_map>
#include <utility>

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
    void visitDefaultInit(DefaultInitExprAST &Node) override {}

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
        checkNode(&Node, Node.getLeftLoc(), Node.getLeftName().length());
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
        checkNode(&Node, Node.getNameLoc(), static_cast<int>(Node.getModuleName().length()));
    }

    // Default implementations for others that we might not fully traverse for MVP
    // but we can add more if needed
};

const ExprAST* SemanticQuery::findNodeAt(const DocumentAnalysis& analysis, int line, int col) {
    NodeFinderVisitor visitor(line, col);
    // Skip the synthetic prelude imports (see DocumentAnalysis::
    // preludeImportCount's doc comment) -- they're not real source the
    // user can click on.
    for (size_t i = analysis.preludeImportCount; i < analysis.asts.size(); ++i) {
        if (analysis.asts[i]) {
            const_cast<ExprAST*>(analysis.asts[i].get())->accept(visitor);
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

std::optional<HoverInfo> SemanticQuery::hover(const ExprAST* node, int cursorLine, int cursorCol) {
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
            return HoverInfo{"```turf\nfn " + target->signature() + "\n```",
                             fnDef->getNameLoc(), bareNameLength(fnDef->getName())};
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
        std::string text = "module `" + scopeAccess->getLeftName() + "`";
        return HoverInfo{text, scopeAccess->getLeftLoc(),
                         static_cast<int>(scopeAccess->getLeftName().length())};
    } else if (auto moduleImport = dynamic_cast<const ModuleImportExprAST*>(node)) {
        std::string text = "module `" + moduleImport->getModuleName() + "`";
        if (!moduleImport->getResolvedPath().empty())
            text += "\n\n" + moduleImport->getResolvedPath();
        return HoverInfo{text, moduleImport->getNameLoc(),
                         static_cast<int>(moduleImport->getModuleName().length())};
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
    for (size_t i = analysis.preludeImportCount; i < analysis.asts.size(); ++i) {
        if (analysis.asts[i]) const_cast<ExprAST*>(analysis.asts[i].get())->accept(indexer);
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
        addToken(Node.getLeftLoc(), Node.getLeftName().length(), SemanticTokenType::Namespace);
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
        addToken(Node.getNameLoc(), Node.getModuleName().length(), SemanticTokenType::Namespace,
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
    for (size_t i = analysis.preludeImportCount; i < analysis.asts.size(); ++i) {
        if (analysis.asts[i]) {
            const_cast<ExprAST*>(analysis.asts[i].get())->accept(visitor);
        }
    }
    
    auto tokens = std::move(visitor.tokens);
    std::sort(tokens.begin(), tokens.end(), [](const SemanticToken& a, const SemanticToken& b) {
        if (a.line != b.line) return a.line < b.line;
        return a.col < b.col;
    });
    return tokens;
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

    void visitVarDecl(VarDeclExprAST &Node) override {
        if (auto sym = Node.getResolvedSymbol()) {
            VariableTypes[Node.getName()] = sym->Type;
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

} // namespace

std::vector<CompletionItem> SemanticQuery::getCompletions(const DocumentAnalysis& analysis,
                                                           const std::string& lineTextBeforeCursor,
                                                           const std::string& documentPath) {
    std::vector<CompletionItem> items;

    TriggerContext ctx = classifyTrigger(lineTextBeforeCursor);

    // `import <prefix>` -- module-name completion (doc 05 G2). Answered
    // from ListAvailableModules() (turf-lang's Parser.h), a pure filesystem
    // lookup that needs no resolved AST at all -- unlike every other branch
    // below, this doesn't require `analysis.resolver` to be non-null, which
    // matters here specifically: an in-progress `import ` line (no `;` yet)
    // is exactly the kind of buffer the patched-reanalysis path may not
    // fully resolve.
    if (ctx.kind == TriggerContext::Kind::Import) {
        for (const auto& name : ListAvailableModules(documentPath)) {
            CompletionItem item;
            item.label = name;
            item.kind = 9; // Module
            item.detail = "module";
            item.insertSnippet = name + ";";
            items.push_back(std::move(item));
        }
        return items;
    }

    if (!analysis.resolver) return items;
    const ResolverSymbolTable& symTable = analysis.resolver->symbolTable();

    if (ctx.kind == TriggerContext::Kind::ScopeAccess) {
        const std::string& base = ctx.base;
        if (symTable.hasModule(base)) {
            for (const auto& name : symTable.getModuleFunctionNames(base)) {
                if (auto sym = firstOverload(symTable, name, base)) {
                    CompletionItem item;
                    item.label = name;
                    item.kind = 3; // Function
                    item.detail = sym->signature();
                    item.insertSnippet = buildCallSnippet(name, *sym, false);
                    items.push_back(std::move(item));
                }
            }
            for (const auto& name : symTable.getModuleTypeNames(base)) {
                items.push_back({name, 22, "type in " + base, ""});
            }
            for (const auto& name : symTable.getModuleConstantNames(base)) {
                items.push_back({name, 21, "constant in " + base, ""});
            }
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
                CompletionItem item;
                item.label = method;
                item.kind = 2; // Method (static)
                item.detail = sym ? sym->signature() : (base + "." + method + "()");
                item.insertSnippet = sym ? buildCallSnippet(method, *sym, false) : (method + "()$0");
                items.push_back(std::move(item));
            }
        }
        return items;
    }

    if (ctx.kind == TriggerContext::Kind::MemberAccess) {
        DocumentIndexVisitor indexer;
        for (const auto& ast : analysis.asts)
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
            items.push_back(std::move(item));
        }

        std::string owner = symTable.getTypeOwningModule(structName);
        for (const auto& entry : structMethods(symTable, structName, owner)) {
            const std::string& method = entry.first;
            const ResolverFunctionSymbol* sym = entry.second;
            if (method == "new" || method == "default") continue; // ConstructorCalledOnInstanceError
            if (!isInstanceMethod(sym)) continue; // instance-only after '.'
            CompletionItem item;
            item.label = method;
            item.kind = 2; // Method
            item.detail = sym->signature();
            item.insertSnippet = buildCallSnippet(method, *sym, true);
            items.push_back(std::move(item));
        }
        return items;
    }

    // Bare word: keywords + types + document variables + current-file
    // functions + known struct/enum names -- a real superset of what's
    // usable unqualified here, instead of the old fixed list.
    static const std::vector<std::string> keywords = {
        "fn", "mut", "immut", "struct", "extern", "from", "import", "export",
        "static", "if", "elseif", "else", "while", "for", "in", "step",
        "give", "otherwise", "break", "continue", "return", "to", "enum"
    };
    static const std::vector<std::string> types = {
        "int", "double", "bool", "string", "ptr", "void"
    };
    for (const auto& kw : keywords) items.push_back({kw, 14, "keyword", ""});
    for (const auto& ty : types) items.push_back({ty, 7, "type", ""});
    items.push_back({"true", 21, "bool", ""});
    items.push_back({"false", 21, "bool", ""});
    items.push_back({"null", 21, "", ""});
    items.push_back({"assert", 3, "Builtin", "assert(${1:condition}, \"${2:message}\")$0"});

    DocumentIndexVisitor indexer;
    for (const auto& ast : analysis.asts)
        if (ast) const_cast<ExprAST*>(ast.get())->accept(indexer);
    for (const auto& entry : indexer.VariableTypes) {
        items.push_back({entry.first, 6, entry.second ? entry.second->getName() : "", ""});
    }
    for (const auto& name : symTable.getAllCurrentFileFunctionNames()) {
        if (name.find('.') != std::string::npos) continue; // struct methods, not bare-callable
        if (auto sym = firstOverload(symTable, name, "")) {
            CompletionItem item;
            item.label = name;
            item.kind = 3;
            item.detail = sym->signature();
            item.insertSnippet = buildCallSnippet(name, *sym, false);
            items.push_back(std::move(item));
        }
    }
    for (const auto& name : symTable.getAllStructNames()) items.push_back({name, 22, "struct", ""});
    for (const auto& name : symTable.getAllEnumNames()) items.push_back({name, 22, "enum", ""});

    return items;
}
