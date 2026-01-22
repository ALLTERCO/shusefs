# Shelly FUSE System Module Makefile

# Platform detection
UNAME_S := $(shell uname -s)

# Compiler and flags - platform specific
ifeq ($(UNAME_S),Darwin)
    # macOS with macFUSE or FUSE-T
    CC = clang
    # Try pkg-config first, fall back to known paths
    FUSE_CFLAGS := $(shell PKG_CONFIG_PATH=/opt/macfuse/lib/pkgconfig:/opt/local/lib/pkgconfig pkg-config --cflags fuse 2>/dev/null || echo "-I/opt/macfuse/include/fuse -D_FILE_OFFSET_BITS=64")
    FUSE_LDFLAGS := $(shell PKG_CONFIG_PATH=/opt/macfuse/lib/pkgconfig:/opt/local/lib/pkgconfig pkg-config --libs fuse 2>/dev/null || echo "-L/opt/macfuse/lib -lfuse -pthread")
    FUSE_VERSION = 29
    PLATFORM_CFLAGS = -D__APPLE__
    PLATFORM_LDFLAGS =
else
    # Linux
    CC = gcc
    FUSE_CFLAGS = $(shell pkg-config --cflags fuse3)
    FUSE_LDFLAGS = $(shell pkg-config --libs fuse3)
    FUSE_VERSION = 31
    PLATFORM_CFLAGS =
    PLATFORM_LDFLAGS = -lpthread
endif

CFLAGS = -Wall -Wextra -O2 -D_FILE_OFFSET_BITS=64 -DFUSE_USE_VERSION=$(FUSE_VERSION) -Iinclude $(FUSE_CFLAGS) $(PLATFORM_CFLAGS)
LDFLAGS = $(PLATFORM_LDFLAGS) $(FUSE_LDFLAGS)

# Directories
PREFIX = /usr/local
BINDIR = $(PREFIX)/bin
SRCDIR = src
INCDIR = include
BUILDDIR = build

# Target executable
TARGET = shusefs

# Source files
SOURCES = $(SRCDIR)/main.c $(SRCDIR)/mongoose.c $(SRCDIR)/request_queue.c $(SRCDIR)/device_state.c $(SRCDIR)/fuse_ops.c
OBJECTS = $(SOURCES:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)

# Default target
all: check-format $(TARGET)

# Build the executable
$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $(TARGET) $(LDFLAGS)

# Create build directory
$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# Compile source files
$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Install the binary
install: $(TARGET)
	install -d $(BINDIR)
	install -m 755 $(TARGET) $(BINDIR)

# Uninstall
uninstall:
	rm -f $(BINDIR)/$(TARGET)

# Clean build artifacts
clean:
	rm -rf $(BUILDDIR)
	rm -f $(TARGET)

# Clean everything including dependencies
distclean: clean
	rm -f *~ *.bak
	rm -f $(SRCDIR)/*~ $(INCDIR)/*~

# Clang tools
format:
	@echo "Formatting source files..."
	@if command -v clang-format >/dev/null 2>&1; then \
		clang-format -i $(SOURCES) $(INCDIR)/*.h; \
		echo "Done."; \
	else \
		echo "Warning: clang-format not found. Skipping formatting."; \
		echo "Install with: sudo dnf install clang-tools-extra"; \
	fi

check-format:
	@if command -v clang-format >/dev/null 2>&1; then \
		echo "Checking code formatting..."; \
		clang-format --dry-run --Werror $(SOURCES) $(INCDIR)/*.h 2>&1 || { \
			echo "Error: Formatting issues found. Run 'make format' to fix."; \
			exit 1; \
		}; \
	else \
		echo "Warning: clang-format not found. Skipping format check."; \
	fi

tidy:
	@if command -v clang-tidy >/dev/null 2>&1; then \
		echo "Running clang-tidy static analysis..."; \
		clang-tidy $(SOURCES) -- $(CFLAGS); \
	else \
		echo "Warning: clang-tidy not found. Skipping static analysis."; \
	fi

# Show detected platform configuration
info:
	@echo "Platform: $(UNAME_S)"
	@echo "Compiler: $(CC)"
	@echo "FUSE version: $(FUSE_VERSION)"
	@echo "FUSE CFLAGS: $(FUSE_CFLAGS)"
	@echo "FUSE LDFLAGS: $(FUSE_LDFLAGS)"
	@echo "CFLAGS: $(CFLAGS)"
	@echo "LDFLAGS: $(LDFLAGS)"

.PHONY: all install uninstall clean distclean format check-format tidy info
