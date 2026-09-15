#pragma once

#include "DocumentAnalysis.hpp"
#include "AST.h"
#include "ResolverSymbolTable.h"
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

    // Formats a hover tooltip for the symbol referenced by the node.
    static std::optional<std::string> hoverText(const ExprAST* node);

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
