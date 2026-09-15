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
    // doc 05 G4 (completionItem/resolve): the flat symbol-table name to
    // re-look-up on resolve (e.g. "printline", "Point.distance" -- the same
    // key structMethods()/firstOverload() already use), and its owning
    // module ("" = current file). Empty `resolveName` means this item has
    // nothing worth resolving lazily (a keyword, a type name, a local
    // variable, ...) -- LSPHandler omits the `data` field entirely for
    // those, so the client never sends a resolve request for them.
    std::string resolveName;
    std::string resolveModule;
    // doc 05 G3/G6: when non-empty, the client sorts the completion LIST
    // by this string instead of falling back to `label` -- lets a lower-
    // priority-but-alphabetically-earlier entry (e.g. a keyword) rank
    // below a more relevant one (e.g. a local variable) that would
    // otherwise sort first purely by spelling. Left empty for a context
    // that's already a small, uniformly-relevant set with no ranking
    // question worth answering (enum variants, import module names, ...).
    std::string sortText;
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

// A single top-level declaration for textDocument/documentSymbol (doc 08
// P3.6). `kind` is an LSP SymbolKind (Function=12, Struct=23, Enum=10,
// Variable=13) -- LSPHandler forwards it as-is.
struct DocumentSymbolEntry {
    std::string name;
    int kind;
    SourceLocation loc;
    int length;
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

// A single occurrence of a symbol (doc 08 -- find references / document
// highlight). `declLoc`/`declModule` together identify WHICH declaration
// this occurrence resolves to: locals/params are always `declModule ==
// ""` (VariableSymbol carries no SourceModule of its own -- a local can
// only ever be declared in the same file it's used in), while a
// function/method occurrence carries whatever module the Resolver
// attributed it to ("" = current file). Comparing two occurrences'
// identity requires resolving each side's `declModule` to an absolute
// file path first (via ResolveModuleFile(), against EACH occurrence's
// OWN containing file as the importing-file context) -- a bare module
// NAME isn't itself comparable across files, only what it resolves to.
struct ReferenceOccurrence {
    SourceLocation loc;
    int length;
    bool isDeclaration;
    ResolverSourceLocation declLoc;
    std::string declModule;
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

    // completionItem/resolve support (doc 05 G4): looks up the declaration
    // site of a function/method by its flat symbol-table name (e.g.
    // "printline" or "Point.distance" -- CompletionItem::resolveName) and
    // owning module ("" = current file, matching resolveModule). Returns
    // nullopt if the name no longer resolves (the document changed between
    // the original completion request and the client's resolve request).
    static std::optional<SymbolLocation> getFunctionDeclLoc(const DocumentAnalysis& analysis,
                                                             const std::string& name,
                                                             const std::string& module);

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

    // Every reference-bearing occurrence (variable use/decl, function
    // call/def, method call) in `analysis`'s own top-level file --
    // deliberately NOT recursing into imported-module bodies, same
    // reasoning SemanticTokensVisitor::visitModuleImport documents (those
    // positions belong to a different file's coordinate space). Struct/
    // enum/field/module-name occurrences aren't tracked yet (doc 08 scope
    // note) -- only the node kinds getDeclarationLoc() already resolves.
    // Used for both documentHighlight (this file only) and as the
    // per-file scan step of workspace-wide references.
    static std::vector<ReferenceOccurrence> findAllReferenceOccurrences(const DocumentAnalysis& analysis);

    // doc 08 P3.6: top-level struct/enum/function declarations in
    // `analysis`'s own file, for a flat (non-nested) documentSymbol
    // outline. Skips synthetic prelude imports and anything declared
    // inside an imported module's body (same "this file only" scoping
    // findAllReferenceOccurrences() uses).
    static std::vector<DocumentSymbolEntry> getDocumentSymbols(const DocumentAnalysis& analysis);

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
    // `cursorLine` (1-indexed, compiler convention; 0 = unknown) drives doc
    // 05 G3's scope-distance ranking for Bare-word local-variable
    // suggestions; every other trigger kind ignores it too.
    static std::vector<CompletionItem> getCompletions(const DocumentAnalysis& analysis,
                                                       const std::string& lineTextBeforeCursor,
                                                       const std::string& documentPath = "",
                                                       int cursorLine = 0);

    // doc 05 G2's `import mod::<prefix>` case: detects the shape purely
    // textually (no analysis needed -- `mod` isn't imported into the
    // current file yet, so nothing in ITS symbol table could answer this
    // anyway) by checking that "import" is the whole word immediately
    // before the `mod::` base, the same word-boundary check the bare
    // `import <prefix>` trigger uses. Returns true and sets
    // `outModuleName` on a match; leaves it untouched otherwise. Callable
    // by LSPHandler BEFORE running any analysis at all, since the answer
    // doesn't depend on one.
    static bool isImportSymbolTrigger(const std::string& lineTextBeforeCursor,
                                      std::string& outModuleName);

    // The functions/structs/enums/constants a module exports, read from an
    // analysis of the MODULE'S OWN file (`moduleAnalysis` -- produced by
    // analyzing that file as if it were the entry point, exactly what
    // `import mod::<prefix>` completion needs: `mod` isn't imported into
    // the CURRENT file yet, so its symbols aren't reachable through that
    // file's own resolved symbol table at all). Reuses the same "current
    // file" queries getCompletions()'s Bare-word branch already uses,
    // just pointed at a different DocumentAnalysis -- when a module is
    // analyzed standalone, ITS own top-level declarations ARE its
    // "current file" symbols.
    static std::vector<CompletionItem> getModuleExportedSymbols(const DocumentAnalysis& moduleAnalysis);
};
