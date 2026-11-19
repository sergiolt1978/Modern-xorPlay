TARGET_ID := 00FF0000A0F1A001

.PHONY: all deps sysmodule control dist clean

all: deps sysmodule control dist

deps:
	@./fetch-deps.sh

sysmodule: deps
	@$(MAKE) -C sysmodule

control:
	@$(MAKE) -C control

dist: sysmodule control
	@rm -rf dist
	@mkdir -p dist/atmosphere/contents/$(TARGET_ID)/flags
	@cp sysmodule/xorplay.nsp dist/atmosphere/contents/$(TARGET_ID)/exefs.nsp
	@touch dist/atmosphere/contents/$(TARGET_ID)/flags/boot2.flag
	@mkdir -p dist/config/xorplay
	@cp config/config.ini dist/config/xorplay/config.ini
	@mkdir -p dist/music/default dist/music/home
	@cp control/xorplay-control.nro dist/switch/xorplay-control.nro
	@cp README.md dist/README.md
	@cp LICENSE dist/LICENSE
	@cd dist && zip -r ../XorPlay-Modern.zip .
	@echo "Created XorPlay-Modern.zip"

clean:
	@$(MAKE) -C sysmodule clean || true
	@$(MAKE) -C control clean || true
	@rm -rf dist XorPlay-Modern.zip
