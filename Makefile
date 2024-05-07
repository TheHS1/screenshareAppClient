PROJECTNAME = remoteDesktopClient
OUTPUT_DIR = build

LIBS = -lavcodec -lavutil -lavformat -lSDL2main -lSDL2 -lSDL2_net

SRC = $(wildcard src/*.cpp)

default:
	g++ ${SRC} -o $(OUTPUT_DIR)/$(PROJECTNAME) $(INCLUDE_DIRS) $(LIBS) -g
