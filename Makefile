PROJECTNAME = remoteDesktopClient
OUTPUT_DIR = build

INCLUDE_DIRS = -Iinclude/
LIBS = -lavcodec -lavutil -lavformat -lSDL2main -lSDL2 -lSDL2_net

SRC = $(wildcard src/*.cpp) $(wildcard imgui/*.cpp)

default:
	g++ ${SRC} -o $(OUTPUT_DIR)/$(PROJECTNAME) $(INCLUDE_DIRS) $(LIBS) -g
