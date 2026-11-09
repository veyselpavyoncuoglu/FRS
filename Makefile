# FRS build for Windows (mingw32-make) and Linux (make). Every src/*.cpp goes in, platform files guard themselves.
SRC ?= src
MANIFEST ?= 1
# a test build (MANIFEST=0) gets its own objects and exe, so its switches never end up in the release FRS.exe
TEST := $(if $(filter 0,$(MANIFEST)),_test)
OBJ ?= build/obj$(TEST)
IMGUI ?= imgui
IMGUI_OBJ ?= build/imgui
CXX := g++

ifeq ($(OS),Windows_NT)
EXE ?= FRS$(TEST).exe
FLAGS := -O2 -std=c++17 -DUNICODE -D_UNICODE -mwindows -static-libgcc -static-libstdc++ -static
BACKENDS := imgui_impl_win32 imgui_impl_dx11
LIBS := -ld3d11 -ldxgi -ld3dcompiler -lpsapi -luser32 -lgdi32 -lshell32 -ldwmapi -ladvapi32 -lws2_32 -lkernel32 \
	-lole32 -loleaut32 -luuid
ifeq ($(MANIFEST),1)
RES := res/app.res
endif
else
EXE ?= FRS$(TEST)
FLAGS := -O2 -std=c++17
BACKENDS := imgui_impl_glfw imgui_impl_opengl3
GLFW_CFLAGS ?= $(shell pkg-config --cflags glfw3 2>/dev/null)
GLFW_LIBS ?= $(shell pkg-config --libs glfw3 2>/dev/null)
ifeq ($(strip $(GLFW_LIBS)),)
GLFW_LIBS := -lglfw
endif
LIBS := $(GLFW_LIBS) -lGL -lpthread -ldl
endif

# a test build : no UAC manifest and the FRS_TEST_* switches, see util::testEnv
ifeq ($(MANIFEST),0)
FLAGS += -DFRS_TEST
endif

INC := -I$(IMGUI) -I$(SRC) $(GLFW_CFLAGS)
OBJS := $(patsubst $(SRC)/%.cpp,$(OBJ)/%.o,$(wildcard $(SRC)/*.cpp))
IOBJS := $(patsubst %,$(IMGUI_OBJ)/%.o,imgui imgui_draw imgui_tables imgui_widgets $(BACKENDS))

# mingw32-make runs recipes through sh when it finds one, else through cmd. Only cmd echoes the quotes back.
ifeq ($(shell echo ''),)
MKDIR = mkdir -p "$(1)"
RMDIR = rm -rf "$(1)"
RMFILE = rm -f "$(1)"
else
MKDIR = if not exist "$(subst /,\,$(1))" mkdir "$(subst /,\,$(1))"
RMDIR = if exist "$(subst /,\,$(1))" rmdir /s /q "$(subst /,\,$(1))"
RMFILE = if exist "$(subst /,\,$(1))" del /q "$(subst /,\,$(1))"
endif

all: $(EXE)

ifeq ($(OS),Windows_NT)
# response file, long absolute paths in the object list can pass cmd's line limit
$(EXE): $(OBJS) $(IOBJS) $(RES)
	$(file >$(OBJ)/link.rsp,$(OBJS) $(IOBJS) $(RES))
	$(call MKDIR,$(patsubst %/,%,$(dir $@)))
	$(CXX) $(FLAGS) @$(OBJ)/link.rsp $(LIBS) -o $@

res/app.res: res/app.rc res/app.manifest res/FRS.ico
	cd res && windres app.rc -O coff -o app.res
else
$(EXE): $(OBJS) $(IOBJS)
	$(call MKDIR,$(patsubst %/,%,$(dir $@)))
	$(CXX) $(FLAGS) $(OBJS) $(IOBJS) $(LIBS) -o $@
endif

$(OBJ)/%.o: $(SRC)/%.cpp | $(OBJ)
	$(CXX) $(FLAGS) $(INC) -MMD -MP -c $< -o $@

$(IMGUI_OBJ)/%.o: $(IMGUI)/%.cpp | $(IMGUI_OBJ)
	$(CXX) $(FLAGS) $(INC) -MMD -MP -c $< -o $@

$(OBJ):
	$(call MKDIR,$@)

# after $(OBJ) so two mkdirs never race on a shared parent
$(IMGUI_OBJ): | $(OBJ)
	$(call MKDIR,$@)

clean:
	$(call RMDIR,$(OBJ))
	$(call RMDIR,$(IMGUI_OBJ))
	$(call RMFILE,$(EXE))

.PHONY: all clean

-include $(OBJS:.o=.d) $(IOBJS:.o=.d)
