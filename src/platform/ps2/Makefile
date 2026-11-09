EE_BIN  = OpenLara.elf
EE_CBIN = OpenLara_comp.elf
IRX_FILES = 
EE_OBJS = ../../libs/stb_vorbis/stb_vorbis.o sprintf.o system.o joy.o audio.o main.o $(IRX_FILES:.irx=_irx.o)

EE_ASM_DIR = asm/

EE_INCS     = -I../../ -I$(PS2SDK)/ports/include -I$(GSKIT)/include
EE_OPTFLAGS = -O2 -pipe -w -Wl,--wrap,printf -Wl,--wrap,sprintf -DSTB_VORBIS_NO_STDIO
EE_CXXFLAGS = -fno-exceptions -fno-rtti -Wno-invalid-offsetof

EE_LDFLAGS  = -L$(PS2SDK)/ports/lib -L$(GSKIT)/lib
EE_LIBS     = -lm -lstdc++ -lkernel -lpad -ldma -lgraph -ldraw -laudsrv -lvux

NEWLIB_NANO = 1

BIN2S = $(PS2SDK)/bin/bin2s

ifeq ($(DEBUG),1)
	EE_OPTFLAGS := $(EE_OPTFLAGS) -D_DEBUG=1 -g3
	EE_LIBS := $(EE_LIBS) -ldebug
endif

all: $(EE_BIN) $(EE_CBIN) $(IRX)

$(EE_CBIN): $(EE_BIN)
	ps2-packer $(EE_BIN) $(EE_CBIN)

%_irx.c:
	$(PS2SDK)/bin/bin2c $(PS2SDK)/iop/irx/$*.irx $@ $(notdir $*)_irx

clean:
	rm -f $(EE_BIN) $(EE_CBIN) $(EE_OBJS) *.s

run: $(EE_CBIN)
	ps2client execee host:$(EE_CBIN) -D

emu: $(EE_BIN)
	cmd.exe /C "emu.cmd $(EE_BIN)"

reset:
	ps2client reset

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal_cpp
