CC = gcc
CXX = g++

# Compiler settings
CFLAGS = -I./portaudio/include -I./portaudio/src/common -I./portaudio/src/os/win -DPA_USE_WASAPI=1 $(SAMPLERATE_INC)
# LDFLAGS = -lwinmm -lole32 -luuid -lsetupapi $(SAMPLERATE_LIB)
LDFLAGS = -static -static-libgcc -static-libstdc++ -lwinmm -lole32 -luuid -lsetupapi -lavrt

# Folders
OBJ_DIR = build

# List of PortAudio source files
PA_SOURCES = portaudio/src/common/pa_front.c \
             portaudio/src/common/pa_allocation.c \
             portaudio/src/common/pa_converters.c \
             portaudio/src/common/pa_process.c \
             portaudio/src/common/pa_stream.c \
             portaudio/src/common/pa_debugprint.c \
             portaudio/src/common/pa_trace.c \
             portaudio/src/common/pa_cpuload.c \
             portaudio/src/common/pa_dither.c \
             portaudio/src/common/pa_ringbuffer.c \
             portaudio/src/os/win/pa_win_hostapis.c \
             portaudio/src/os/win/pa_win_util.c \
             portaudio/src/os/win/pa_win_coinitialize.c \
             portaudio/src/os/win/pa_win_waveformat.c \
             portaudio/src/os/win/pa_win_version.c \
             portaudio/src/hostapi/wasapi/pa_win_wasapi.c

# Convert source file paths to object file paths
PA_OBJECTS = $(addprefix $(OBJ_DIR)/, $(notdir $(PA_SOURCES:.c=.o)))
MAIN_OBJECT = $(OBJ_DIR)/main.o

# Default target
all: $(OBJ_DIR) main.exe

# Create build directory
$(OBJ_DIR):
	@if not exist $(OBJ_DIR) mkdir $(OBJ_DIR)

# Link everything
main.exe: $(PA_OBJECTS) $(MAIN_OBJECT)
	$(CXX) $^ $(LDFLAGS) -o $@

# Compile main.cpp
$(MAIN_OBJECT): main.cpp
	$(CXX) -c $< -I./portaudio/include $(SAMPLERATE_INC) -o $@

# Compile PortAudio C files
$(OBJ_DIR)/%.o: portaudio/src/common/%.c
	$(CC) -c $< $(CFLAGS) -o $@

$(OBJ_DIR)/%.o: portaudio/src/os/win/%.c
	$(CC) -c $< $(CFLAGS) -o $@

$(OBJ_DIR)/%.o: portaudio/src/hostapi/wasapi/%.c
	$(CC) -c $< $(CFLAGS) -o $@

clean:
	@if exist $(OBJ_DIR) rmdir /s /q $(OBJ_DIR)
	@if exist main.exe del main.exe