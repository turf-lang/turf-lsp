#pragma once

#include "DocumentAnalysis.hpp"
#include "AST.h"
#include "ResolverSymbolTable.h"
#include <optional>
#include <string>
#include <vector>

// One suggestion offered back to the editor. Deliberately not nlohmann::json
// directly -- keeps SemanticQuery's completion logic testable/independent of
// the wire format; LSPHandler does the final JSON-RPC serialization.
struct CompletionItem {
    std::string label;
    // LSP CompletionItemKind (see the LSP spec) -- Method=2, Function=3,
    // Field=5, Variable=6, Class=7 (used here for struct/type names),
    // Module=9, Keyword=14, EnumMember=20, Constant=21, Struct=22.
    int kind = 6;
    std::string detail;
    // When non-empty, used as the completion's insertText with
    // insertTextFormat=2 (Snippet, e.g. "method(${1:arg})$0") instead of a
    // plain-text insertion of `label`.
    std::string insertSnippet;
};

enum class SemanticTokenType {
    Variable = 0,
    Function = 1,
    Keyword = 2,
    Type = 3,
    Parameter = 4,
    Struct = 5,
    String = 6,
    Number = 7,
    Operator = 8,
    Namespace = 9,  // module names, both `import <name>` and the left side of `::`
    Property = 10,  // struct field access (`.field`) and struct field decls
    EnumMember = 11, // the right side of an enum `::` access
    Method = 12     // the right side of a `.method(...)` call
};

enum class SemanticTokenModifier {
    Declaration = 1 << 0,
    Readonly = 1 << 1
};

struct SemanticToken {
    int line;
    int col;
    int length;
    SemanticTokenType type;
    int modifiers;
};

struct SymbolLocation {
    ResolverSourceLocation loc;
    std::string moduleName;
    // Length of the target's own name at `loc`, for a precise
    // `targetSelectionRange` (doc 07 P2.5) instead of a 1-char caret.
    // Defaults to 1 for the rare case a caller doesn't know it better.
    int length = 1;
};

// A hover result: the rendered markdown plus the span of the hovered token
// itself (doc 06 P2.1's `range`, so the client underlines exactly the
// symbol rather than nothing). `length` is 0 (never rendered as a range by
// the caller) when the exact span isn't confidently known.
struct HoverInfo {
    std::string markdown;
    SourceLocation loc;
    int length = 0;
};

class SemanticQuery {
public:
    // Finds the innermost AST node that intersects with the given Line and Col.
    // Both Line and Col are 1-indexed. Hit-tests name-bearing nodes (struct/fn/
    // extern/var decls, field/method access) against their own getNameLoc()
    // (issue #230) rather than a preceding keyword/operator's location, so no
    // source-text recovery is needed here.
    static const ExprAST* findNodeAt(const DocumentAnalysis& analysis, int line, int col);

    // Returns the location of the declaration for the symbol referenced by the node,
    // along with the module name if it's imported.
    static std::optional<SymbolLocation> getDeclarationLoc(const ExprAST* node);

    // textDocument/typeDefinition: from a variable/expression to the
    // declaration of its *type* (struct or enum), not the variable itself.
    // Needs `analysis` (not just the node) to build the struct/enum-name ->
    // decl-location index, including names declared in transitively
    // imported modules (walked via each ModuleImportExprAST's already-
    // parsed getBody(), no re-analysis of the target file needed). Returns
    // the module name the type is owned by ("" for the current file), same
    // shape as getDeclarationLoc(), for the caller to resolve a target URI.
    static std::optional<SymbolLocation> getTypeDefinitionLoc(const DocumentAnalysis& analysis,
                                                               const ExprAST* node);

    // Formats a hover tooltip (+ its own source range) for the symbol
    // referenced by `node`. `cursorLine`/`cursorCol` (1-indexed, same as
    // findNodeAt) are needed only to disambiguate a ScopeAccessExprAST's two
    // names (`Left::Right`) sharing one node -- every other node kind
    // ignores them. `analysis` is needed only for a FuncDefExprAST with
    // multiple overloads (doc 06's overload-listing edge case) -- every
    // other node kind ignores it too.
    static std::optional<HoverInfo> hover(const DocumentAnalysis& analysis, const ExprAST* node,
                                          int cursorLine, int cursorCol);

    // Extracts semantic tokens from the given document analysis. Name-bearing
    // nodes (struct/fn/extern/var decls, field/method access, module imports)
    // are colored at their own getNameLoc() (issue #230), not a preceding
    // keyword/operator's location.
    static std::vector<SemanticToken> getSemanticTokens(const DocumentAnalysis& analysis);

    // Builds context-aware completion items for a cursor sitting right after
    // `lineTextBeforeCursor` (the current line's text up to, not including,
    // the cursor). Classifies the trigger (bare word / `Ident::` / `Ident.`)
    // from that text alone, then answers it using `analysis`'s resolved
    // symbol table and AST -- real module functions/types after `io::`, real
    // struct fields/methods after `instance.`, real in-scope
    // variables/functions for a bare word, instead of a fixed keyword list.
    // `documentPath` is the on-disk (or virtual) path of the document being
    // completed -- used only for Import-context completion (`import
    // <prefix>`), which needs it to resolve sibling `.tr` files next to the
    // document via ListAvailableModules(). Every other trigger kind ignores it.
    static std::vector<CompletionItem> getCompletions(const DocumentAnalysis& analysis,
                                                       const std::string& lineTextBeforeCursor,
                                                       const std::string& documentPath = "");
};
