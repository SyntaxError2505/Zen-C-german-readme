# Compiler configuration
# Default: gcc
# To build with clang: make CC=clang
# To build with zig:   make CC="zig cc"
CC = gcc
CFLAGS = -Wall -Wextra -g -I./src -I./src/ast -I./src/parser -I./src/codegen -I./plugins -I./src/zen -I./src/utils -I./src/lexer -I./src/analysis -I./src/lsp
TARGET = zc
LIBS = -lm -lpthread -ldl

SRCS = src/main.c \
       src/parser/parser_core.c \
       src/parser/parser_expr.c \
       src/parser/parser_stmt.c \
       src/parser/parser_type.c \
       src/parser/parser_utils.c \
       src/ast/ast.c \
       src/codegen/codegen.c \
       src/codegen/codegen_decl.c \
       src/codegen/codegen_main.c \
       src/codegen/codegen_utils.c \
       src/utils/utils.c \
       src/lexer/token.c \
       src/analysis/typecheck.c \
       src/lsp/json_rpc.c \
       src/lsp/lsp_main.c \
       src/lsp/lsp_analysis.c \
       src/lsp/lsp_index.c \
       src/lsp/lsp_project.c \
       src/zen/zen_facts.c \
       src/repl/repl.c \
       src/plugins/plugin_manager.c

OBJ_DIR = obj
OBJS = $(patsubst %.c, $(OBJ_DIR)/%.o, $(SRCS))

# Installation paths
PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin
MANDIR = $(PREFIX)/share/man
SHAREDIR = $(PREFIX)/share/zenc
INCLUDEDIR = $(PREFIX)/include/zenc

PLUGINS = plugins/befunge.so plugins/brainfuck.so plugins/forth.so plugins/lisp.so plugins/regex.so plugins/sql.so

# Default target
all: $(TARGET) $(PLUGINS)

# Build plugins
plugins/%.so: plugins/%.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $<

# Link
$(TARGET): $(OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)
	@echo "=> Build complete: $(TARGET)"

# Compile
$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Install
install: $(TARGET)
	install -d $(BINDIR)
	install -m 755 $(TARGET) $(BINDIR)/$(TARGET)
	
	# Install man pages
	install -d $(MANDIR)/man1 $(MANDIR)/man5 $(MANDIR)/man7
	test -f man/zc.1 && install -m 644 man/zc.1 $(MANDIR)/man1/zc.1 || true
	test -f man/zc.5 && install -m 644 man/zc.5 $(MANDIR)/man5/zc.5 || true
	test -f man/zc.7 && install -m 644 man/zc.7 $(MANDIR)/man7/zc.7 || true
	
	# Install standard library
	install -d $(SHAREDIR)
	cp -r std $(SHAREDIR)/
	
	# Install plugin headers
	install -d $(INCLUDEDIR)
	install -m 644 plugins/zprep_plugin.h $(INCLUDEDIR)/zprep_plugin.h
	@echo "=> Installed to $(BINDIR)/$(TARGET)"
	@echo "=> Man pages installed to $(MANDIR)"
	@echo "=> Standard library installed to $(SHAREDIR)/std"

# Uninstall
uninstall:
	rm -f $(BINDIR)/$(TARGET)
	rm -f $(MANDIR)/man1/zc.1
	rm -f $(MANDIR)/man5/zc.5
	rm -f $(MANDIR)/man7/zc.7
	rm -rf $(SHAREDIR)
	@echo "=> Uninstalled from $(BINDIR)/$(TARGET)"
	@echo "=> Removed man pages from $(MANDIR)"
	@echo "=> Removed $(SHAREDIR)"

# Clean
clean:
	rm -rf $(OBJ_DIR) $(TARGET) out.c plugins/*.so
	@echo "=> Clean complete!"

# Test
test: $(TARGET)
	./tests/run_tests.sh
	./tests/run_codegen_tests.sh

# Build with alternative compilers
zig:
	$(MAKE) CC="zig cc"

clang:
	$(MAKE) CC=clang

.PHONY: all clean install uninstall test zig clang
