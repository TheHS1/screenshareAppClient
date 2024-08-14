PROJECTNAME = remoteDesktopClient
OUTPUT_DIR = build

INCLUDE_DIRS = -Iinclude/
LIBS = -lavcodec -lavutil -lavformat -lSDL2main -lSDL2 -lSDL2_net -lcrypto -lssl

SRC = $(wildcard src/*.cpp) $(wildcard imgui/*.cpp)

default:
	g++ ${SRC} -o $(OUTPUT_DIR)/$(PROJECTNAME) $(INCLUDE_DIRS) $(LIBS) -g

install:
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f $(OUTPUT_DIR)/$(PROJECTNAME) $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/$(PROJECTNAME)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(PROJECTNAME)
