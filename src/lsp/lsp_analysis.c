#include "json_rpc.h"
#include "lsp_project.h" // Includes lsp_index.h, parser.h
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct Diagnostic
{
    int line;
    int col;
    char *message;
    struct Diagnostic *next;
} Diagnostic;

typedef struct
{
    Diagnostic *head;
    Diagnostic *tail;
} DiagnosticList;

// Callback for parser errors.
void lsp_on_error(void *data, Token t, const char *msg)
{
    DiagnosticList *list = (DiagnosticList *)data;
    // Simple allocation for MVP
    Diagnostic *d = calloc(1, sizeof(Diagnostic));
    d->line = t.line > 0 ? t.line - 1 : 0;
    d->col = t.col > 0 ? t.col - 1 : 0;
    d->message = strdup(msg);
    d->next = NULL;

    if (!list->head)
    {
        list->head = d;
        list->tail = d;
    }
    else
    {
        list->tail->next = d;
        list->tail = d;
    }
}

void lsp_check_file(const char *uri, const char *json_src)
{
    if (!g_project)
    {
        // Fallback or lazy init? current dir
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd)))
        {
            lsp_project_init(cwd);
        }
        else
        {
            lsp_project_init(".");
        }
    }

    // Setup error capture on the global project context
    DiagnosticList diagnostics = {0};

    // We attach the callback to 'g_project->ctx'.
    // NOTE: If we use lsp_project_update_file, it uses g_project->ctx.
    void *old_data = g_project->ctx->error_callback_data;
    void (*old_cb)(void *, Token, const char *) = g_project->ctx->on_error;

    g_project->ctx->error_callback_data = &diagnostics;
    g_project->ctx->on_error = lsp_on_error;

    // Update and Parse
    lsp_project_update_file(uri, json_src);

    // Restore
    g_project->ctx->on_error = old_cb;
    g_project->ctx->error_callback_data = old_data;

    // Construct JSON Response (publishDiagnostics)
    char *notification = malloc(128 * 1024);
    char *p = notification;
    p += sprintf(p,
                 "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/"
                 "publishDiagnostics\",\"params\":{\"uri\":\"%s\",\"diagnostics\":[",
                 uri);

    Diagnostic *d = diagnostics.head;
    while (d)
    {
        p += sprintf(p,
                     "{\"range\":{\"start\":{\"line\":%d,\"character\":%d},\"end\":"
                     "{\"line\":%d,"
                     "\"character\":%d}},\"severity\":1,\"message\":\"%s\"}",
                     d->line, d->col, d->line, d->col + 1, d->message);

        if (d->next)
        {
            p += sprintf(p, ",");
        }
        d = d->next;
    }

    p += sprintf(p, "]}}");

    long len = strlen(notification);
    fprintf(stdout, "Content-Length: %ld\r\n\r\n%s", len, notification);
    fflush(stdout);

    free(notification);

    Diagnostic *cur = diagnostics.head;
    while (cur)
    {
        Diagnostic *next = cur->next;
        free(cur->message);
        free(cur);
        cur = next;
    }
}

void lsp_goto_definition(const char *uri, int line, int col)
{
    ProjectFile *pf = lsp_project_get_file(uri);
    LSPIndex *idx = pf ? pf->index : NULL;

    if (!idx)
    {
        return;
    }

    LSPRange *r = lsp_find_at(idx, line, col);

    // 1. Check Local Index
    if (r)
    {
        if (r->type == RANGE_DEFINITION)
        {
            // Already at definition
            char resp[1024];
            sprintf(resp,
                    "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"uri\":\"%s\","
                    "\"range\":{\"start\":{"
                    "\"line\":%d,\"character\":%d},\"end\":{\"line\":%d,\"character\":%d}}}}",
                    uri, r->start_line, r->start_col, r->end_line, r->end_col);
            fprintf(stdout, "Content-Length: %ld\r\n\r\n%s", strlen(resp), resp);
            fflush(stdout);
            return;
        }
        else if (r->type == RANGE_REFERENCE && r->def_line >= 0)
        {
            LSPRange *def = lsp_find_at(idx, r->def_line, r->def_col);
            int is_local = 0;
            if (def && def->type == RANGE_DEFINITION)
            {
                // Check name congruence
                char *ref_name = NULL;
                char *def_name = NULL;

                if (r->node->type == NODE_EXPR_VAR)
                {
                    ref_name = r->node->var_ref.name;
                }
                else if (r->node->type == NODE_EXPR_CALL &&
                         r->node->call.callee->type == NODE_EXPR_VAR)
                {
                    ref_name = r->node->call.callee->var_ref.name;
                }

                if (def->node->type == NODE_FUNCTION)
                {
                    def_name = def->node->func.name;
                }
                else if (def->node->type == NODE_VAR_DECL)
                {
                    def_name = def->node->var_decl.name;
                }
                else if (def->node->type == NODE_CONST)
                {
                    def_name = def->node->var_decl.name;
                }
                else if (def->node->type == NODE_STRUCT)
                {
                    def_name = def->node->strct.name;
                }

                if (ref_name && def_name && strcmp(ref_name, def_name) == 0)
                {
                    is_local = 1;
                }
            }

            if (is_local)
            {
                char resp[1024];
                sprintf(resp,
                        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"uri\":\"%s\","
                        "\"range\":{\"start\":{"
                        "\"line\":%d,\"character\":%d},\"end\":{\"line\":%d,\"character\":%d}}}}",
                        uri, r->def_line, r->def_col, r->def_line, r->def_col);
                fprintf(stdout, "Content-Length: %ld\r\n\r\n%s", strlen(resp), resp);
                fflush(stdout);
                return;
            }
        }
    }

    // 2. Global Definition (if local failed)
    if (r && r->node)
    {
        char *name = NULL;
        if (r->node->type == NODE_EXPR_VAR)
        {
            name = r->node->var_ref.name;
        }
        else if (r->node->type == NODE_EXPR_CALL && r->node->call.callee->type == NODE_EXPR_VAR)
        {
            name = r->node->call.callee->var_ref.name;
        }

        if (name)
        {
            DefinitionResult def = lsp_project_find_definition(name);
            if (def.uri && def.range)
            {
                char resp[1024];
                sprintf(resp,
                        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"uri\":\"%s\","
                        "\"range\":{\"start\":{"
                        "\"line\":%d,\"character\":%d},\"end\":{\"line\":%d,\"character\":%d}}}}",
                        def.uri, def.range->start_line, def.range->start_col, def.range->end_line,
                        def.range->end_col);
                fprintf(stdout, "Content-Length: %ld\r\n\r\n%s", strlen(resp), resp);
                fflush(stdout);
                return;
            }
        }
    }

    const char *null_resp = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":null}";
    fprintf(stdout, "Content-Length: %ld\r\n\r\n%s", strlen(null_resp), null_resp);
    fflush(stdout);
}

void lsp_hover(const char *uri, int line, int col)
{
    (void)uri;
    ProjectFile *pf = lsp_project_get_file(uri);
    LSPIndex *idx = pf ? pf->index : NULL;

    if (!idx)
    {
        return;
    }

    LSPRange *r = lsp_find_at(idx, line, col);
    char *text = NULL;

    if (r)
    {
        if (r->type == RANGE_DEFINITION)
        {
            text = r->hover_text;
        }
        else if (r->type == RANGE_REFERENCE)
        {
            LSPRange *def = lsp_find_at(idx, r->def_line, r->def_col);
            if (def && def->type == RANGE_DEFINITION)
            {
                text = def->hover_text;
            }
        }
    }

    if (text)
    {
        char *json = malloc(16384);
        sprintf(json,
                "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"contents\":{\"kind\":"
                "\"markdown\","
                "\"value\":\"```c\\n%s\\n```\"}}}",
                text);

        fprintf(stdout, "Content-Length: %ld\r\n\r\n%s", strlen(json), json);
        fflush(stdout);
        free(json);
    }
    else
    {
        const char *null_resp = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":null}";
        fprintf(stdout, "Content-Length: %ld\r\n\r\n%s", strlen(null_resp), null_resp);
        fflush(stdout);
    }
}

void lsp_completion(const char *uri, int line, int col)
{
    ProjectFile *pf = lsp_project_get_file(uri);
    // Need global project context
    if (!g_project || !g_project->ctx || !pf)
    {
        return;
    }

    // 1. Context-aware completion (Dot access)
    if (pf->source)
    {
        int cur_line = 0;
        char *ptr = pf->source;
        while (*ptr && cur_line < line)
        {
            if (*ptr == '\n')
            {
                cur_line++;
            }
            ptr++;
        }

        if (col > 0 && ptr[col - 1] == '.')
        {
            // Found dot! Scan backwards for identifier
            int i = col - 2;
            while (i >= 0 && (ptr[i] == ' ' || ptr[i] == '\t'))
            {
                i--;
            }
            if (i >= 0)
            {
                int end_ident = i;
                while (i >= 0 && (isalnum(ptr[i]) || ptr[i] == '_'))
                {
                    i--;
                }
                int start_ident = i + 1;

                if (start_ident <= end_ident)
                {
                    int len = end_ident - start_ident + 1;
                    char var_name[256];
                    strncpy(var_name, ptr + start_ident, len);
                    var_name[len] = 0;

                    char *type_name = NULL;
                    Symbol *sym = find_symbol_in_all(g_project->ctx, var_name);

                    if (sym)
                    {
                        if (sym->type_info)
                        {
                            type_name = type_to_string(sym->type_info);
                        }
                        else if (sym->type_name)
                        {
                            type_name = sym->type_name;
                        }
                    }

                    if (type_name)
                    {
                        char clean_name[256];
                        char *src = type_name;
                        if (strncmp(src, "struct ", 7) == 0)
                        {
                            src += 7;
                        }
                        char *dst = clean_name;
                        while (*src && *src != '*')
                        {
                            *dst++ = *src++;
                        }
                        *dst = 0;

                        // Lookup struct in GLOBAL registry
                        StructDef *sd = g_project->ctx->struct_defs;
                        while (sd)
                        {
                            if (strcmp(sd->name, clean_name) == 0)
                            {
                                // Found struct!
                                char *json_fields = malloc(1024 * 1024);
                                char *pj = json_fields;
                                pj += sprintf(pj, "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":[");

                                int ffirst = 1;
                                if (sd->node && sd->node->strct.fields)
                                {
                                    ASTNode *field = sd->node->strct.fields;
                                    while (field)
                                    {
                                        if (!ffirst)
                                        {
                                            pj += sprintf(pj, ",");
                                        }
                                        pj += sprintf(
                                            pj,
                                            "{\"label\":\"%s\",\"kind\":5,\"detail\":\"field %s\"}",
                                            field->field.name, field->field.type);
                                        ffirst = 0;
                                        field = field->next;
                                    }
                                }
                                pj += sprintf(pj, "]}");
                                fprintf(stdout, "Content-Length: %ld\r\n\r\n%s",
                                        strlen(json_fields), json_fields);
                                fflush(stdout);
                                free(json_fields);
                                free(type_name); // type_to_string arg
                                return;
                            }
                            sd = sd->next;
                        }
                        if (sym && sym->type_info)
                        {
                            free(type_name);
                        }
                    }
                }
            }
        }
    }

    // 2. Global Completion (Functions & Structs)
    char *json = malloc(1024 * 1024);
    char *p = json;
    p += sprintf(p, "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":[");

    int first = 1;

    // Functions
    FuncSig *f = g_project->ctx->func_registry;
    while (f)
    {
        if (!first)
        {
            p += sprintf(p, ",");
        }
        p += sprintf(p, "{\"label\":\"%s\",\"kind\":3,\"detail\":\"fn %s\"}", f->name, f->name);
        first = 0;
        f = f->next;
    }

    // Structs
    StructDef *s = g_project->ctx->struct_defs;
    while (s)
    {
        if (!first)
        {
            p += sprintf(p, ",");
        }
        p +=
            sprintf(p, "{\"label\":\"%s\",\"kind\":22,\"detail\":\"struct %s\"}", s->name, s->name);
        first = 0;
        s = s->next;
    }

    p += sprintf(p, "]}");
    fprintf(stdout, "Content-Length: %ld\r\n\r\n%s", strlen(json), json);
    fflush(stdout);
    free(json);
}

void lsp_document_symbol(const char *uri)
{
    ProjectFile *pf = lsp_project_get_file(uri);
    if (!pf || !pf->index)
    {
        const char *null_resp = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":null}";
        fprintf(stdout, "Content-Length: %ld\r\n\r\n%s", strlen(null_resp), null_resp);
        fflush(stdout);
        return;
    }

    char *json = malloc(1024 * 1024);
    char *p = json;
    p += sprintf(p, "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":[");

    int first = 1;
    LSPRange *r = pf->index->head;
    while (r)
    {
        if (r->type == RANGE_DEFINITION && r->node)
        {
            char *name = NULL;
            int kind = 0; // 0 = default

            if (r->node->type == NODE_FUNCTION)
            {
                name = r->node->func.name;
                kind = 12; // Function
            }
            else if (r->node->type == NODE_STRUCT)
            {
                name = r->node->strct.name;
                kind = 23; // Struct
            }
            else if (r->node->type == NODE_VAR_DECL)
            {
                name = r->node->var_decl.name;
                kind = 13; // Variable
            }
            else if (r->node->type == NODE_CONST)
            {
                name = r->node->var_decl.name;
                kind = 14; // Constant
            }

            if (name)
            {
                if (!first)
                {
                    p += sprintf(p, ",");
                }
                p += sprintf(p,
                             "{\"name\":\"%s\",\"kind\":%d,\"location\":{\"uri\":\"%s\",\"range\":{"
                             "\"start\":{\"line\":%d,\"character\":%d},\"end\":{\"line\":%d,"
                             "\"character\":%d}}}}",
                             name, kind, uri, r->start_line, r->start_col, r->end_line, r->end_col);
                first = 0;
            }
        }
        r = r->next;
    }

    p += sprintf(p, "]}");
    fprintf(stdout, "Content-Length: %ld\r\n\r\n%s", strlen(json), json);
    fflush(stdout);
    free(json);
}

void lsp_references(const char *uri, int line, int col)
{
    ProjectFile *pf = lsp_project_get_file(uri);
    if (!pf || !pf->index)
    {
        return;
    }

    LSPRange *r = lsp_find_at(pf->index, line, col);
    if (!r || !r->node)
    {
        // No symbol at cursor?
        const char *null_resp = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":[]}";
        fprintf(stdout, "Content-Length: %ld\r\n\r\n%s", strlen(null_resp), null_resp);
        fflush(stdout);
        return;
    }

    char *name = NULL;
    if (r->node->type == NODE_FUNCTION)
    {
        name = r->node->func.name;
    }
    else if (r->node->type == NODE_VAR_DECL)
    {
        name = r->node->var_decl.name;
    }
    else if (r->node->type == NODE_CONST)
    {
        name = r->node->var_decl.name;
    }
    else if (r->node->type == NODE_STRUCT)
    {
        name = r->node->strct.name;
    }
    else if (r->node->type == NODE_EXPR_VAR)
    {
        name = r->node->var_ref.name;
    }
    else if (r->node->type == NODE_EXPR_CALL && r->node->call.callee->type == NODE_EXPR_VAR)
    {
        name = r->node->call.callee->var_ref.name;
    }

    if (!name)
    {
        const char *null_resp = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":[]}";
        fprintf(stdout, "Content-Length: %ld\r\n\r\n%s", strlen(null_resp), null_resp);
        fflush(stdout);
        return;
    }

    ReferenceResult *refs = lsp_project_find_references(name);

    char *json = malloc(1024 * 1024); // Large buffer for references
    char *p = json;
    p += sprintf(p, "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":[");

    int first = 1;
    ReferenceResult *curr = refs;
    while (curr)
    {
        if (!first)
        {
            p += sprintf(p, ",");
        }
        p += sprintf(p,
                     "{\"uri\":\"%s\",\"range\":{\"start\":{\"line\":%d,\"character\":%d},\"end\":{"
                     "\"line\":%d,\"character\":%d}}}",
                     curr->uri, curr->range->start_line, curr->range->start_col,
                     curr->range->end_line, curr->range->end_col);
        first = 0;

        ReferenceResult *next = curr->next;
        free(curr); // Free linked list node as we go
        curr = next;
    }

    p += sprintf(p, "]}");
    fprintf(stdout, "Content-Length: %ld\r\n\r\n%s", strlen(json), json);
    fflush(stdout);
    free(json);
}

void lsp_signature_help(const char *uri, int line, int col)
{
    ProjectFile *pf = lsp_project_get_file(uri);
    if (!g_project || !g_project->ctx || !pf || !pf->source)
    {
        return;
    }

    // Scan backwards from cursor for '('
    char *ptr = pf->source;
    int cur_line = 0;
    while (*ptr && cur_line < line)
    {
        if (*ptr == '\n')
        {
            cur_line++;
        }
        ptr++;
    }

    // We are at start of line. Advance to col.
    if (ptr && col > 0)
    {
        ptr += col;
    }

    // Safety check
    if (ptr > pf->source + strlen(pf->source))
    {
        return;
    }

    // Scan backwards
    char *p = ptr - 1;
    while (p >= pf->source)
    {
        if (*p == ')')
        {
            // Nested call or closed. Bail for simple implementation.
            // Or skip balanced parens (TODO for better robustness)
            return;
        }
        if (*p == '(')
        {
            // Found open paren!
            // Look for identifier before it.
            char *ident_end = p - 1;
            while (ident_end >= pf->source && isspace(*ident_end))
            {
                ident_end--;
            }

            if (ident_end < pf->source)
            {
                return;
            }

            char *ident_start = ident_end;
            while (ident_start >= pf->source && (isalnum(*ident_start) || *ident_start == '_'))
            {
                ident_start--;
            }
            ident_start++;

            // Extract name
            int len = ident_end - ident_start + 1;
            if (len <= 0 || len > 255)
            {
                return;
            }

            char func_name[256];
            strncpy(func_name, ident_start, len);
            func_name[len] = 0;

            // Lookup Function
            FuncSig *fn = g_project->ctx->func_registry;
            while (fn)
            {
                if (strcmp(fn->name, func_name) == 0)
                {
                    char *json = malloc(4096);
                    char label[2048];
                    // Reconstruct signature label
                    char params[1024] = "";
                    int first = 1;

                    // Use total_args and arg_types
                    for (int i = 0; i < fn->total_args; i++)
                    {
                        if (!first)
                        {
                            strcat(params, ", ");
                        }

                        // Convert Type* to string
                        char *tstr = type_to_string(fn->arg_types[i]);
                        if (tstr)
                        {
                            strcat(params, tstr);
                            free(tstr);
                        }
                        else
                        {
                            strcat(params, "unknown");
                        }

                        first = 0;
                    }

                    char *ret_str = type_to_string(fn->ret_type);
                    sprintf(label, "fn %s(%s) -> %s", fn->name, params, ret_str ? ret_str : "void");
                    if (ret_str)
                    {
                        free(ret_str);
                    }

                    sprintf(json,
                            "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"signatures\":[{\"label\":"
                            "\"%s\",\"parameters\":[]}]}}",
                            label);

                    fprintf(stdout, "Content-Length: %ld\r\n\r\n%s", strlen(json), json);
                    fflush(stdout);
                    free(json);
                    return;
                }
                fn = fn->next;
            }
            break; // Found paren but no func match
        }
        p--;
    }

    const char *null_resp = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":null}";
    fprintf(stdout, "Content-Length: %ld\r\n\r\n%s", strlen(null_resp), null_resp);
    fflush(stdout);
}
