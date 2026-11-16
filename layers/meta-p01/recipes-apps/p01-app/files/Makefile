# ----------------------------------------------------------------------------------------------------
# p01-app - build in the PetaLinux / Yocto recipe (p01-app.bb): CXX, CXXFLAGS and LDFLAGS come from
# the recipe environment, libraries and headers from the recipe sysroot.
# p01_preproc_ref.hpp (shared with the HLS testbench) is fetched next to this Makefile.
# ----------------------------------------------------------------------------------------------------
TARGET := p01-app
SRCS   := $(wildcard src/*.cpp)
OBJS   := $(SRCS:.cpp=.o)

CXXFLAGS += -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -DEGL_NO_X11 -I src -I .
LDLIBS   := -lvart-runner -lvart-util -lxir -lunilog -lglog -lEGL -lGLESv2 -ljpeg -lpthread

$(TARGET): $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

src/%.o: src/%.cpp $(wildcard src/*.hpp) p01_preproc_ref.hpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: clean
