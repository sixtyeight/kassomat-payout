# -*- mode: Makefile; -*-
# -----------------------------------------
# project itlssptest


CFLAGS_C = $(filter-out -include "sdk.h",$(CFLAGS))

# -----------------------------------------

# MAKE_DEP = -MMD -MT $@ -MF $(@:.o=.d)

CFLAGS = -Wall -g -O0
INCLUDES = 
LDFLAGS = -s
RCFLAGS = 
\LDLIBS = $(T_LDLIBS)  -lstdc++ -lpthread
LDLIBS = $(T_LDLIBS)  -lpthread -lhiredis -levent -luuid -ljansson

LINK_exe = gcc -o $@ $^ $(LDFLAGS) $(LDLIBS)
LINK_con = gcc -o $@ $^ $(LDFLAGS) $(LDLIBS)
LINK_dll = gcc -o $@ $^ $(LDFLAGS) $(LDLIBS) -shared
LINK_lib = rm -f $@ && ar rcs $@ $^
COMPILE_c = gcc $(CFLAGS_C) -o $@ -c $< $(MAKEDEP) $(INCLUDES)
COMPILE_cpp = g++ $(CFLAGS) -o $@ -c $< $(MAKEDEP) $(INCLUDES)
COMPILE_rc = windres $(RCFLAGS) -J rc -O coff -i $< -o $@ -I$(dir $<)

%.o : %.c ; $(COMPILE_c)
%.o : %.cpp ; $(COMPILE_cpp)
%.o : %.cxx ; $(COMPILE_cpp)
%.o : %.rc ; $(COMPILE_rc)
.SUFFIXES: .o .d .c .cpp .cxx .rc

all: all.before all.targets all.after

all.before :
	$(MAKE) -C libitlssp
all.after : $(FIRST_TARGET)

all.targets : Release_target 

clean :
	$(MAKE) -C libitlssp clean
	rm -fv $(clean.OBJ)
	rm -fv $(DEP_FILES)

.PHONY: all clean distclean

# -----------------------------------------
# Release_target

Release_target.BIN = payoutd 
Release_target.OBJ = payoutd.o linux.o
DEP_FILES += payoutd.d 
clean.OBJ += $(Release_target.BIN) $(Release_target.OBJ)

Release_target : Release_target.before $(Release_target.BIN) Release_target.after_always
Release_target : CFLAGS += -g -O0
Release_target : INCLUDES += 
Release_target : RCFLAGS += 
Release_target : LDFLAGS += -s
Release_target : T_LDLIBS = ./libitlssp/bin/libitlssp.a 
ifdef LMAKE
Release_target : CFLAGS -= -g -pipe
endif

Release_target.before :


Release_target.after_always : $(Release_target.BIN)

$(Release_target.BIN) : $(Release_target.OBJ)
	$(LINK_con)
	
# -----------------------------------------
ifdef MAKE_DEP
-include $(DEP_FILES)
endif
