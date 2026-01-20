#ifndef LSP_PROJECT_H
#define LSP_PROJECT_H

#include "parser.h"
#include "lsp_index.h"

typedef struct ProjectFile
{
    char *path;      // Absolute path
    char *uri;       // file:// URI
    char *source;    // Cached source content
    LSPIndex *index; // File-specific index (local vars, refs)
    struct ProjectFile *next;
} ProjectFile;

typedef struct
{
    // Global symbol table (Structs, Functions, Globals)
    // We reuse ParserContext for this, as it already supports registries.
    ParserContext *ctx;

    // List of tracked files
    ProjectFile *files;

    // Root directory
    char *root_path;
} LSPProject;

// Global project instance
extern LSPProject *g_project;

// Initialize the project with a root directory
void lsp_project_init(const char *root_path);

// Find a file in the project
ProjectFile *lsp_project_get_file(const char *uri);

// Update a file (re-parse and re-index)
void lsp_project_update_file(const char *uri, const char *src);

// Find definition globally
typedef struct
{
    char *uri;
    LSPRange *range;
} DefinitionResult;

DefinitionResult lsp_project_find_definition(const char *name);

typedef struct ReferenceResult
{
    char *uri;
    LSPRange *range;
    struct ReferenceResult *next;
} ReferenceResult;

ReferenceResult *lsp_project_find_references(const char *name);

#endif
