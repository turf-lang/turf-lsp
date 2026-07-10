#include "SemanticQuery.hpp"
#include "ASTVisitor.h"

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
        checkNode(&Node, Node.getLoc(), Node.getName().length());
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
        checkNode(&Node, Node.getLoc(), Node.getName().length());
        if (Node.getBodyMut()) const_cast<ExprAST*>(Node.getBodyMut())->accept(*this);
    }
    
    void visitFuncCall(FuncCallExprAST &Node) override {
        if (Node.getCallee()) const_cast<ExprAST*>(Node.getCallee())->accept(*this);
        for (const auto& arg : Node.getArgs()) {
            if (arg) const_cast<ExprAST*>(arg.get())->accept(*this);
        }
    }

    // Default implementations for others that we might not fully traverse for MVP
    // but we can add more if needed
};

const ExprAST* SemanticQuery::findNodeAt(const DocumentAnalysis& analysis, int line, int col) {
    NodeFinderVisitor visitor(line, col);
    for (const auto& ast : analysis.asts) {
        if (ast) {
            const_cast<ExprAST*>(ast.get())->accept(visitor);
        }
    }
    return visitor.getBestNode();
}

std::optional<ResolverSourceLocation> SemanticQuery::getDeclarationLoc(const ExprAST* node) {
    if (!node) return std::nullopt;

    if (auto varNode = dynamic_cast<const VariableExprAST*>(node)) {
        if (auto sym = varNode->getResolvedSymbol()) {
            return sym->DeclLoc;
        }
    } else if (auto varDecl = dynamic_cast<const VarDeclExprAST*>(node)) {
        if (auto sym = varDecl->getResolvedSymbol()) {
            return sym->DeclLoc;
        }
    } else if (auto callNode = dynamic_cast<const FuncCallExprAST*>(node)) {
        if (auto target = callNode->getResolvedCallTarget()) {
            return target->DeclLoc;
        }
    } else if (auto fnDef = dynamic_cast<const FuncDefExprAST*>(node)) {
        if (auto target = fnDef->getResolvedDef()) {
            return target->DeclLoc;
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
    }
    return std::nullopt;
}
