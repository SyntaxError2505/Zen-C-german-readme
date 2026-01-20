
#include "json_rpc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// Helper to skip whitespace
const char *skip_ws(const char *p)
{
    while (*p && isspace(*p))
    {
        p++;
    }
    return p;
}

// Robust JSON string extractor
// Finds "key" ... : ... "value"
char *get_json_string(const char *json, const char *key)
{
    char key_pattern[256];
    sprintf(key_pattern, "\"%s\"", key);

    char *p = strstr(json, key_pattern);
    while (p)
    {
        const char *cursor = p + strlen(key_pattern);
        cursor = skip_ws(cursor);
        if (*cursor == ':')
        {
            cursor++; // skip :
            cursor = skip_ws(cursor);
            if (*cursor == '"')
            {
                // Found start of value
                cursor++;
                const char *start = cursor;
                // Find end " (handling escapes?)
                // MVP: just find next " that is not escaped
                while (*cursor)
                {
                    if (*cursor == '"' && *(cursor - 1) != '\\')
                    {
                        break;
                    }
                    cursor++;
                }

                int len = cursor - start;
                char *res = malloc(len + 1);
                strncpy(res, start, len);
                res[len] = 0;
                return res;
            }
        }
        // False positive? Find next
        p = strstr(p + 1, key_pattern);
    }
    return NULL;
}

// Extract nested "text" from params...
// Since "text" might be huge and contain anything, we need to be careful.
char *get_text_content(const char *json)
{
    // Search for "text" key
    char *res = get_json_string(json, "text");
    if (!res)
    {
        return NULL;
    }

    size_t len = strlen(res);
    char *unescaped = malloc(len + 1);
    char *dst = unescaped;
    char *src = res;
    while (*src)
    {
        if (*src == '\\')
        {
            src++;
            if (*src == 'n')
            {
                *dst++ = '\n';
            }
            else if (*src == 'r')
            {
                *dst++ = '\r';
            }
            else if (*src == 't')
            {
                *dst++ = '\t';
            }
            else if (*src == '"')
            {
                *dst++ = '"';
            }
            else if (*src == '\\')
            {
                *dst++ = '\\';
            }
            else
            {
                *dst++ = *src;
            }
        }
        else
        {
            *dst++ = *src;
        }
        src++;
    }
    *dst = 0;
    free(res);
    return unescaped;
}

void get_json_position(const char *json, int *line, int *col)
{
    // Search for "line" and "character"
    // Note: they are integers

    char *p = strstr(json, "\"line\"");
    if (p)
    {
        p += 6; // skip "line"
        p = (char *)skip_ws(p);
        if (*p == ':')
        {
            p++;
            p = (char *)skip_ws(p);
            *line = atoi(p);
        }
    }

    p = strstr(json, "\"character\"");
    if (p)
    {
        p += 11;
        p = (char *)skip_ws(p);
        if (*p == ':')
        {
            p++;
            p = (char *)skip_ws(p);
            *col = atoi(p);
        }
    }
}

void lsp_check_file(const char *uri, const char *src);
void lsp_goto_definition(const char *uri, int line, int col);
void lsp_hover(const char *uri, int line, int col);
void lsp_completion(const char *uri, int line, int col);
void lsp_document_symbol(const char *uri);
void lsp_references(const char *uri, int line, int col);
void lsp_signature_help(const char *uri, int line, int col);

#include "lsp_project.h"

void handle_request(const char *json_str)
{
    // Looser method detection
    if (strstr(json_str, "initialize"))
    {
        // Extract rootPath or rootUri
        char *root = get_json_string(json_str, "rootPath");
        if (!root)
        {
            root = get_json_string(json_str, "rootUri");
        }

        // Clean up URI if needed
        if (root && strncmp(root, "file://", 7) == 0)
        {
            char *clean = strdup(root + 7);
            free(root);
            root = clean;
        }

        if (root)
        {
            lsp_project_init(root);
            free(root);
        }
        else
        {
            lsp_project_init(".");
        }

        const char *response = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{"
                               "\"capabilities\":{\"textDocumentSync\":1,"
                               "\"definitionProvider\":true,\"hoverProvider\":true,"
                               "\"completionProvider\":{"
                               "\"triggerCharacters\":[\".\"]}}}}";
        fprintf(stdout, "Content-Length: %ld\r\n\r\n%s", strlen(response), response);
        fflush(stdout);
        return;
    }

    if (strstr(json_str, "didOpen") || strstr(json_str, "didChange"))
    {
        char *uri = get_json_string(json_str, "uri");
        char *text = get_text_content(json_str);

        if (uri && text)
        {
            // fprintf(stderr, "zls: Checking %s\n", uri);
            lsp_check_file(uri, text);
        }

        if (uri)
        {
            free(uri);
        }
        if (text)
        {
            free(text);
        }
    }

    if (strstr(json_str, "textDocument/definition"))
    {
        char *uri = get_json_string(json_str, "uri");
        int line = 0, col = 0;
        get_json_position(json_str, &line, &col);

        if (uri)
        {
            lsp_goto_definition(uri, line, col);
            free(uri);
        }
    }

    if (strstr(json_str, "textDocument/hover"))
    {
        char *uri = get_json_string(json_str, "uri");
        int line = 0, col = 0;
        get_json_position(json_str, &line, &col);

        if (uri)
        {
            lsp_hover(uri, line, col);
            free(uri);
        }
    }

    if (strstr(json_str, "textDocument/completion"))
    {
        char *uri = get_json_string(json_str, "uri");
        int line = 0, col = 0;
        get_json_position(json_str, &line, &col);

        if (uri)
        {
            lsp_completion(uri, line, col);
            free(uri);
        }
    }

    if (strstr(json_str, "textDocument/documentSymbol"))
    {
        char *uri = get_json_string(json_str, "uri");
        if (uri)
        {
            lsp_document_symbol(uri);
            free(uri);
        }
    }

    if (strstr(json_str, "textDocument/references"))
    {
        char *uri = get_json_string(json_str, "uri");
        int line = 0, col = 0;
        get_json_position(json_str, &line, &col);

        if (uri)
        {
            lsp_references(uri, line, col);
            free(uri);
        }
    }

    if (strstr(json_str, "textDocument/signatureHelp"))
    {
        char *uri = get_json_string(json_str, "uri");
        int line = 0, col = 0;
        get_json_position(json_str, &line, &col);

        if (uri)
        {
            lsp_signature_help(uri, line, col);
            free(uri);
        }
    }
}
