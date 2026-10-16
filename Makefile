CXX ?= g++
CPPFLAGS += -I/usr/local/include
CXXFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c++11
LDFLAGS += -L/usr/local/lib
LDLIBS += -lrpitx -lm -lrt -lpthread -lbcm_host

TARGET := build/gpio4tuningd

.PHONY: all clean install
all: $(TARGET)

$(TARGET): src/gpio4tuningd.cpp
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LDFLAGS) $(LDLIBS) -o $@

install: $(TARGET)
	sudo install -D -m 0755 $(TARGET) /usr/local/bin/gpio4tuningd
	sudo install -D -m 0644 systemd/gpio4tuningd.service /etc/systemd/system/gpio4tuningd.service
	sudo systemctl daemon-reload
	sudo systemctl enable --now gpio4tuningd.service

clean:
	rm -rf build
