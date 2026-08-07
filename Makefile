
VPATH    = . whisper whisper/virtual_memory

CXX      ?= g++
OFLAGS   = -g
override CXXFLAGS += -std=c++20 -Wall -Wextra -pedantic $(OFLAGS)
override CXXFLAGS += -MMD -MP
override CXXFLAGS += -Iwhisper
SRCS     = DvMmu.cpp VirtMem.cpp Tlb.cpp
OBJS     = $(SRCS:.cpp=.o)

# List of all auto-genreated dependency files.
DEPS_FILES := $(OBJS:.o=.d) sample.d

libdvmmu.a: $(OBJS)
	$(AR) rcs $@ $(OBJS)

sample: sample.o libdvmmu.a
	$(CXX) $(CXXFLAGS) -o $@ $^

# Include Generated Dependency files if available.
-include $(DEPS_FILES)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	$(RM) $(OBJS) libdvmmu.a $(DEPS_FILES) sample.o sample

.PHONY:  iommu virtual_memory tests
