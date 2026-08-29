# TinyVoice ESP32 firmware — PlatformIO shortcuts
#
# Examples:
#   make build
#   make upload
#   make upload PORT=/dev/cu.SLAB_USBtoUART
#   make monitor
#   make flash          # upload + serial monitor
#   make devices
#   make chip-id

PIO        ?= $(shell command -v pio 2>/dev/null || command -v platformio 2>/dev/null)
MONITOR_BAUD ?= 115200

# Override when auto-detection picks the wrong port:
#   make upload PORT=/dev/cu.usbserial-0001
PORT ?=

# Auto-detect common ESP32 USB-serial adapters (macOS / Linux)
AUTO_PORT := $(shell $(PIO) device list 2>/dev/null \
	| grep -E '/dev/(cu|tty)\.(usb|SLAB|wchusbserial|USB)' \
	| head -1 \
	| awk '{print $$1}')

EFFECTIVE_PORT := $(if $(PORT),$(PORT),$(AUTO_PORT))

UPLOAD_FLAGS := $(if $(EFFECTIVE_PORT),--upload-port $(EFFECTIVE_PORT),)
MONITOR_FLAGS := $(if $(EFFECTIVE_PORT),--port $(EFFECTIVE_PORT),)

.PHONY: help build upload monitor clean devices chip-id config setup flash check-pio check-port

help:
	@echo "TinyVoice firmware targets:"
	@echo ""
	@echo "  make setup      Copy config.example.h -> config.h (if missing)"
	@echo "  make build      Compile firmware"
	@echo "  make upload     Build and flash to ESP32"
	@echo "  make monitor    Open serial monitor (115200 baud)"
	@echo "  make flash      Upload, then open serial monitor"
	@echo "  make clean      Remove build artifacts"
	@echo "  make devices    List connected serial ports"
	@echo "  make chip-id    Read chip info via esptool (needs USB connected)"
	@echo ""
	@echo "Options:"
	@echo "  PORT=/dev/cu.xxx   Force serial port (see 'make devices')"
	@echo ""
	@echo "Detected port: $(if $(EFFECTIVE_PORT),$(EFFECTIVE_PORT),none — plug in the board and run 'make devices')"

check-pio:
	@if [ -z "$(PIO)" ]; then \
		echo "PlatformIO not found. Install: https://platformio.org/install/cli"; \
		exit 1; \
	fi

check-port:
	@if [ -z "$(EFFECTIVE_PORT)" ]; then \
		echo "No serial port found. Connect the ESP32 via USB and run 'make devices'."; \
		echo "Then retry with: make upload PORT=/dev/cu.YOUR_PORT"; \
		exit 1; \
	fi

setup: config

config:
	@if [ ! -f include/config.h ]; then \
		cp include/config.example.h include/config.h; \
		echo "Created include/config.h — edit Wi-Fi, API URL, and device token."; \
	else \
		echo "include/config.h already exists."; \
	fi

build: check-pio
	$(PIO) run

upload: check-pio check-port build
	@echo "Uploading to $(EFFECTIVE_PORT) ..."
	$(PIO) run -t upload $(UPLOAD_FLAGS)

monitor: check-pio check-port
	$(PIO) device monitor -b $(MONITOR_BAUD) $(MONITOR_FLAGS)

flash: upload monitor

clean: check-pio
	$(PIO) run -t clean

devices: check-pio
	$(PIO) device list

chip-id: check-pio check-port
	@echo "Reading chip on $(EFFECTIVE_PORT) ..."
	$(PIO) pkg exec -p "tool-esptoolpy" -- esptool.py --port $(EFFECTIVE_PORT) chip_id flash_id
