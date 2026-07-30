#include "SemanticQuery.hpp"
#include "ASTVisitor.h"
#include "Resolver.h"
#include <cctype>
#include <iostream>
#include <unordered_map>
#include <utility>

// Shared by findNameLoc() below and the completion-context scanning further
// down this file.
static bool isIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// A whole class of AST nodes (ModuleImportExprAST, StructDeclExprAST,
// FuncDefExprAST, ExternFuncDeclExprAST, VarDeclExprAST, FieldAccessExprAST,
// MethodCallExprAST) only store ONE location each: the DECLARING KEYWORD's
// (or, for field/method access, the '.' operator's) own position, captured
// in the parser before that token is even consumed -- there's no separate
// field anywhere for the NAME token that follows it. Reusing that position
// as the start of a span sized to the name's length points at the wrong
// text entirely (e.g. `struct E` would highlight/hit-test the 1-character
// span right after 's' -- the first letter of "struct" -- instead of "E").
// Used by both SemanticTokensVisitor (coloring) and NodeFinderVisitor
// (hover/go-to-definition hit-testing), which is why it's a shared free
// function rather than a member of either.
//
// Recovers the name's real column by searching the actual source line for
// it, starting AT (not skipping past) the keyword/operator's own column --
// since `name` can never equal a reserved keyword, the first match at or
// after that column is necessarily the real name token, regardless of what
// (or how long) precedes it. Boundary-checked so a short name doesn't match
// as a substring of something longer (e.g. a struct named "E" wrongly
// matching inside a field type "Entity" later on the same line). Returns
// nullopt if the line/text isn't available or the name genuinely isn't
// found -- better to skip/miss than to act on the wrong text.
static std::optional<SourceLocation> findNameLoc(const std::vector<std::string>* sourceLines,
                                                  const SourceLocation& anchorLoc,
                                                  const std::string& name) {
    if (!sourceLines || name.empty()) return std::nullopt;
    if (anchorLoc.Line <= 0 || static_cast<size_t>(anchorLoc.Line) > sourceLines->size())
        return std::nullopt;
    const std::string& lineText = (*sourceLines)[anchorLoc.Line - 1];
    size_t searchFrom = static_cast<size_t>(anchorLoc.Col > 0 ? anchorLoc.Col - 1 : 0);
    size_t pos = searchFrom;
    while (true) {
        pos = lineText.find(name, pos);
        if (pos == std::string::npos) return std::nullopt;
        bool boundaryBefore = (pos == 0) || !isIdentChar(lineText[pos - 1]);
        size_t afterPos = pos + name.size();
        bool boundaryAfter = (afterPos >= lineText.size()) || !isIdentChar(lineText[afterPos]);
        if (boundaryBefore && boundaryAfter) {
            return SourceLocation{anchorLoc.Line, static_cast<int>(pos + 1), anchorLoc.Filename};
        }
        pos += 1;
    }
}

// A visitor that traverses the AST to find the tightest node enclosing a location.
class NodeFinderVisitor : public ASTVisitor {
    int TargetLine;
    int TargetCol;
    const ExprAST* BestNode = nullptr;
    const std::vector<std::string>* SourceLines = nullptr;

    bool contains(const SourceLocation& Loc, int Length) const {
        if (Loc.Line != TargetLine) return false;
        return TargetCol >= Loc.Col && TargetCol < Loc.Col + Length;
    }

    void checkNode(const ExprAST* Node, const SourceLocation& Loc, int Length) {
        if (contains(Loc, Length)) {
            BestNode = Node;
        }
    }

    // For the same node types findNameLoc() documents (struct/fn/extern/var
    // decls, field/method access): checkNode() against the raw getLoc() +
    // name.length() would test containment against the wrong span (the
    // keyword's or '.'s position, not the name's) -- clicking/hovering
    // directly on the real name would silently miss. Recovers the name's
    // real location first, same as SemanticTokensVisitor does for coloring.
    void checkNamedNode(const ExprAST* Node, const SourceLocation& anchorLoc, const std::string& name) {
        if (auto nameLoc = findNameLoc(SourceLines, anchorLoc, name)) {
            checkNode(Node, *nameLoc, static_cast<int>(name.length()));
        }
    }

public:
    NodeFinderVisitor(int line, int col, const std::vector<std::string>* sourceLines)
        : TargetLine(line), TargetCol(col), SourceLines(sourceLines) {}

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
        checkNamedNode(&Node, Node.getLoc(), Node.getName());
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
        checkNamedNode(&Node, Node.getLoc(), Node.getName());
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
        checkNamedNode(&Node, Node.getLoc(), Node.getMethod());
        for (const auto& arg : Node.getArgs()) {
            if (arg) const_cast<ExprAST*>(arg.get())->accept(*this);
        }
    }

    void visitFieldAccess(FieldAccessExprAST &Node) override {
        if (Node.getBase()) const_cast<ExprAST*>(Node.getBase())->accept(*this);
        checkNamedNode(&Node, Node.getLoc(), Node.getFieldName());
    }

    void visitScopeAccess(ScopeAccessExprAST &Node) override {
        checkNode(&Node, Node.getLeftLoc(), Node.getLeftName().length());
        checkNode(&Node, Node.getRightLoc(), Node.getRightName().length());
    }

    void visitStructDecl(StructDeclExprAST &Node) override {
        checkNamedNode(&Node, Node.getLoc(), Node.getName());
    }

    void visitArrayAccess(ArrayAccessExprAST &Node) override {
        if (Node.getBase()) const_cast<ExprAST*>(Node.getBase())->accept(*this);
        if (Node.getIndex()) const_cast<ExprAST*>(Node.getIndex())->accept(*this);
    }

    // Default implementations for others that we might not fully traverse for MVP
    // but we can add more if needed
};

const ExprAST* SemanticQuery::findNodeAt(const DocumentAnalysis& analysis, int line, int col,
                                         const std::vector<std::string>& sourceLines) {
    NodeFinderVisitor visitor(line, col, &sourceLines);
    for (const auto& ast : analysis.asts) {
        if (ast) {
            const_cast<ExprAST*>(ast.get())->accept(visitor);
        }
    }
    return visitor.getBestNode();
}

std::optional<SymbolLocation> SemanticQuery::getDeclarationLoc(const ExprAST* node) {
    if (!node) return std::nullopt;

    if (auto varNode = dynamic_cast<const VariableExprAST*>(node)) {
        if (auto sym = varNode->getResolvedSymbol()) {
            return SymbolLocation{sym->DeclLoc, ""};
        }
    } else if (auto varDecl = dynamic_cast<const VarDeclExprAST*>(node)) {
        if (auto sym = varDecl->getResolvedSymbol()) {
            return SymbolLocation{sym->DeclLoc, ""};
        }
    } else if (auto callNode = dynamic_cast<const FuncCallExprAST*>(node)) {
        if (auto target = callNode->getResolvedCallTarget()) {
            return SymbolLocation{target->DeclLoc, target->SourceModule};
        }
    } else if (auto fnDef = dynamic_cast<const FuncDefExprAST*>(node)) {
        if (auto target = fnDef->getResolvedDef()) {
            return SymbolLocation{target->DeclLoc, target->SourceModule};
        }
    } else if (auto methodNode = dynamic_cast<const MethodCallExprAST*>(node)) {
        if (auto target = methodNode->getResolvedCallTarget()) {
            return SymbolLocation{target->DeclLoc, target->SourceModule};
        }
    }
    return std::nullopt;
}

std::optional<std::string> SemanticQuery::hoverText(const ExprAST* node) {
    if (!node) return std::nullopt;

    if (auto varNode = dynamic_cast<const VariableExprAST*>(node)) {
        if (auto sym = varNode->getResolvedSymbol()) {
            std::string text = "```turf\n";
            text += (sym->IsImmutable ? "immut " : "mut ");
            text += sym->Name + ": ";
            text += (sym->Type ? sym->Type->getName() : "unknown");
            text += "\n```";
            return text;
        }
    } else if (auto varDecl = dynamic_cast<const VarDeclExprAST*>(node)) {
        if (auto sym = varDecl->getResolvedSymbol()) {
            std::string text = "```turf\n";
            text += (sym->IsImmutable ? "immut " : "mut ");
            text += sym->Name + ": ";
            text += (sym->Type ? sym->Type->getName() : "unknown");
            text += "\n```";
            return text;
        }
    } else if (auto callNode = dynamic_cast<const FuncCallExprAST*>(node)) {
        if (auto target = callNode->getResolvedCallTarget()) {
            return "```turf\nfn " + target->signature() + "\n```";
        }
    } else if (auto fnDef = dynamic_cast<const FuncDefExprAST*>(node)) {
        if (auto target = fnDef->getResolvedDef()) {
            return "```turf\nfn " + target->signature() + "\n```";
        }
    } else if (auto methodNode = dynamic_cast<const MethodCallExprAST*>(node)) {
        if (auto target = methodNode->getResolvedCallTarget()) {
            return "```turf\nfn " + target->signature() + "\n```";
        }
    } else if (auto structDecl = dynamic_cast<const StructDeclExprAST*>(node)) {
        std::string text = "```turf\nstruct " + structDecl->getName() + " {\n";
        for (const auto& field : structDecl->getFields()) {
            text += "    " + field.Name + ": " + (field.Type ? field.Type->getName() : "unknown") + "\n";
        }
        text += "}\n```";
        return text;
    }
    return std::nullopt;
}

class SemanticTokensVisitor : public ASTVisitor {
public:
    std::vector<SemanticToken> tokens;
    const std::vector<std::string>* SourceLines = nullptr;

    void addToken(const SourceLocation& loc, int length, SemanticTokenType type, int modifiers = 0) {
        if (loc.Line > 0 && length > 0) {
            tokens.push_back({loc.Line, loc.Col, length, type, modifiers});
        }
    }

    void visitVariable(VariableExprAST &Node) override {
        int modifiers = 0;
        if (auto sym = Node.getResolvedSymbol()) {
            if (sym->IsImmutable) modifiers |= static_cast<int>(SemanticTokenModifier::Readonly);
        }
        addToken(Node.getLoc(), Node.getName().length(), SemanticTokenType::Variable, modifiers);
    }
    
    void visitVarDecl(VarDeclExprAST &Node) override {
        int modifiers = static_cast<int>(SemanticTokenModifier::Declaration);
        if (Node.isImmutable()) {
            modifiers |= static_cast<int>(SemanticTokenModifier::Readonly);
        }
        // getLoc() is `mut`/`immut`/`mutable`/`immutable`'s own position, not
        // the variable name's -- see findNameLoc()'s doc comment.
        if (auto nameLoc = findNameLoc(SourceLines, Node.getLoc(), Node.getName())) {
            addToken(*nameLoc, Node.getName().length(), SemanticTokenType::Variable, modifiers);
        }
        if (Node.getInitializer()) {
            const_cast<ExprAST*>(Node.getInitializer())->accept(*this);
        }
    }

    void visitFuncDef(FuncDefExprAST &Node) override {
        int modifiers = static_cast<int>(SemanticTokenModifier::Declaration);
        // getLoc() is `fn`'s own position, not the function name's.
        if (auto nameLoc = findNameLoc(SourceLines, Node.getLoc(), Node.getName())) {
            addToken(*nameLoc, Node.getName().length(), SemanticTokenType::Function, modifiers);
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

    // Struct name at its declaration site. Field types aren't separate
    // ExprAST nodes (StructField is a plain name+TurfTypePtr pair), so
    // there's nothing further to recurse into here.
    void visitStructDecl(StructDeclExprAST &Node) override {
        // getLoc() is `struct`'s own position, not the struct name's.
        if (auto nameLoc = findNameLoc(SourceLines, Node.getLoc(), Node.getName())) {
            addToken(*nameLoc, Node.getName().length(), SemanticTokenType::Struct,
                     static_cast<int>(SemanticTokenModifier::Declaration));
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
        // getLoc() is the '.' token's own position (ParsePostfixExpression
        // captures it there and never threads the field-name token's own
        // location through to the AST node at all), not the field name's.
        if (auto nameLoc = findNameLoc(SourceLines, Node.getLoc(), Node.getFieldName())) {
            addToken(*nameLoc, Node.getFieldName().length(), SemanticTokenType::Property);
        }
    }

    // Base.method(args) -- Method is highlighted distinctly from a Property
    // access and from a free Function call.
    void visitMethodCall(MethodCallExprAST &Node) override {
        if (Node.getBase()) const_cast<ExprAST*>(Node.getBase())->accept(*this);
        // getLoc() is the '.' token's own position, same as FieldAccessExprAST.
        if (auto nameLoc = findNameLoc(SourceLines, Node.getLoc(), Node.getMethod())) {
            addToken(*nameLoc, Node.getMethod().length(), SemanticTokenType::Method);
        }
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
        if (auto nameLoc = findNameLoc(SourceLines, Node.getLoc(), Node.getModuleName())) {
            addToken(*nameLoc, Node.getModuleName().length(), SemanticTokenType::Namespace,
                     static_cast<int>(SemanticTokenModifier::Declaration));
        }
    }

    void visitExternFuncDecl(ExternFuncDeclExprAST &Node) override {
        // getLoc() is `extern`'s own position, not the function name's.
        if (auto nameLoc = findNameLoc(SourceLines, Node.getLoc(), Node.getName())) {
            addToken(*nameLoc, Node.getName().length(), SemanticTokenType::Function,
                     static_cast<int>(SemanticTokenModifier::Declaration));
        }
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

std::vector<SemanticToken> SemanticQuery::getSemanticTokens(const DocumentAnalysis& analysis,
                                                             const std::vector<std::string>& sourceLines) {
    SemanticTokensVisitor visitor;
    visitor.SourceLines = &sourceLines;
    for (const auto& ast : analysis.asts) {
        if (ast) {
            const_cast<ExprAST*>(ast.get())->accept(visitor);
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
    enum class Kind { ScopeAccess, MemberAccess, Bare };
    Kind kind;
    std::string base;   // identifier before '::' or '.'; empty for Bare
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
                                                           const std::string& lineTextBeforeCursor) {
    std::vector<CompletionItem> items;
    if (!analysis.resolver) return items;
    const ResolverSymbolTable& symTable = analysis.resolver->symbolTable();

    TriggerContext ctx = classifyTrigger(lineTextBeforeCursor);

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
