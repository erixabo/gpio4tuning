CXX ?= g++
LIBRPITX_PREFIX ?= /usr/local
CPPFLAGS += -I$(LIBRPITX_PREFIX)/include
CXXFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c++11
LDFLAGS += -L$(LIBRPITX_PREFIX)/lib
LDLIBS += -lrpitx -lm -lrt -lpthread -lbcm_host

TARGET := build/gpio4tuningd

.PHONY: all check-librpitx clean install
all: $(TARGET)

check-librpitx:
	@test -r "$(LIBRPITX_PREFIX)/include/librpitx/ngfmdmasync.h" || { \
		echo "Missing librpitx header: $(LIBRPITX_PREFIX)/include/librpitx/ngfmdmasync.h" >&2; \
		echo "Install it from https://github.com/F5OEO/librpitx (see README.md)." >&2; \
		exit 1; \
	}
	@test -r "$(LIBRPITX_PREFIX)/lib/librpitx.a" -o -r "$(LIBRPITX_PREFIX)/lib/librpitx.so" || { \
		echo "Missing librpitx library: expected librpitx.a or librpitx.so under $(LIBRPITX_PREFIX)/lib" >&2; \
		echo "Build and install librpitx from https://github.com/F5OEO/librpitx (see README.md)." >&2; \
		exit 1; \
	}

$(TARGET): src/gpio4tuningd.cpp | check-librpitx
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LDFLAGS) $(LDLIBS) -o $@

install: $(TARGET)
	sudo install -D -m 0755 $(TARGET) /usr/local/bin/gpio4tuningd
	sudo install -D -m 0644 systemd/gpio4tuningd.service /etc/systemd/system/gpio4tuningd.service
	sudo systemctl daemon-reload
	sudo systemctl enable --now gpio4tuningd.service

clean:
	rm -rf build
