# The same build as build.py, for make: make -> build/tinynes.bin with the test cartridge (run from t32/).
# make GAMES=1 after `python3 ../library.py ROM_FOLDER`: the boot screen + game menu.
LLVM ?= /opt/homebrew/opt/llvm/bin
LLD ?= /opt/homebrew/opt/lld/bin
RT = rt
CART ?= ../testcart.nes
ARCH = --target=riscv32-unknown-elf -march=rv32im -mabi=ilp32
CFLAGS = $(ARCH) -O2 -ffreestanding -nostdlibinc -I$(RT)/include -Wall -DNES_SCREEN=0xF0010000u
ifdef GAMES
CARTO = build/games.o
CFLAGS += -DGAMES -I../build
else
CARTO = build/cart.o
endif
build/tinynes.bin: build/crt0.o build/main.o build/nes.o $(CARTO) build/libc.o build/builtins.o
	$(LLD)/ld.lld -T $(RT)/link.ld --gc-sections -o build/tinynes.elf $^
	$(LLVM)/llvm-objcopy -O binary build/tinynes.elf $@
build:
	mkdir -p build
build/crt0.o: $(RT)/crt0.S | build
	$(LLVM)/clang $(ARCH) -c $< -o $@
build/libc.o: $(RT)/libc.c | build
	$(LLVM)/clang $(CFLAGS) -c $< -o $@
build/builtins.o: $(RT)/builtins.c | build
	$(LLVM)/clang $(CFLAGS) -c $< -o $@
build/main.o: main.c ../nes.h | build
	$(LLVM)/clang $(CFLAGS) -c $< -o $@
build/nes.o: ../nes.c ../nes.h | build
	$(LLVM)/clang $(CFLAGS) -c $< -o $@
build/cart.o: cart.S $(CART) | build
	$(LLVM)/clang $(ARCH) -DCART='"$(abspath $(CART))"' -c $< -o $@
build/games.o: ../build/games.S | build
	$(LLVM)/clang $(ARCH) -c $< -o $@
clean:
	rm -rf build
