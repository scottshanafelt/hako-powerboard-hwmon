# SPDX-License-Identifier: GPL-2.0-or-later
#
# Top-level deploy/build Makefile for hako-powerboard-hwmon.
# Module sources live in src/.

PACKAGE_NAME    := hako-powerboard
PACKAGE_VERSION := $(shell awk -F= '/^PACKAGE_VERSION/{gsub(/"/,"",$$2); print $$2}' dkms.conf)

DESTDIR ?=
UDEVDIR ?= $(DESTDIR)/etc/udev/rules.d

DKMS_SRC := /usr/src/$(PACKAGE_NAME)-$(PACKAGE_VERSION)
REPO_DIR := $(abspath .)

.PHONY: help all module module-clean \
        install uninstall \
        install-udev uninstall-udev \
        dkms-install dkms-uninstall

all: help

help:
	@echo "Build:"
	@echo "  module          out-of-tree kernel module build (no install)"
	@echo "  module-clean    clean the out-of-tree build"
	@echo
	@echo "Install (root):"
	@echo "  install         install udev rule + register & build via DKMS"
	@echo "  uninstall       reverse of install"
	@echo "  install-udev    just the udev rule"
	@echo "  dkms-install    just the DKMS register + build + install"
	@echo "  dkms-uninstall  remove from DKMS tree (and the symlink under /usr/src)"

module:
	$(MAKE) -C src

module-clean:
	$(MAKE) -C src clean

install: install-udev dkms-install

uninstall: dkms-uninstall uninstall-udev

install-udev:
	install -d $(UDEVDIR)
	install -m 0644 udev/99-hako-powerboard.rules $(UDEVDIR)/99-hako-powerboard.rules
	-udevadm control --reload

uninstall-udev:
	rm -f $(UDEVDIR)/99-hako-powerboard.rules
	-udevadm control --reload

# Symlink /usr/src/<pkg>-<ver> at the current checkout so DKMS rebuilds
# pick up the latest source after a `git pull` without copying.
dkms-install:
	@if [ -e $(DKMS_SRC) ] && [ ! -L $(DKMS_SRC) ]; then \
		echo "$(DKMS_SRC) exists and is not a symlink — refusing to overwrite" >&2; \
		exit 1; \
	fi
	@if [ "$$(readlink $(DKMS_SRC) 2>/dev/null)" != "$(REPO_DIR)" ]; then \
		rm -f $(DKMS_SRC); \
		ln -s $(REPO_DIR) $(DKMS_SRC); \
		echo "linked $(DKMS_SRC) -> $(REPO_DIR)"; \
	fi
	-dkms remove $(PACKAGE_NAME)/$(PACKAGE_VERSION) --all
	-dkms add $(PACKAGE_NAME)/$(PACKAGE_VERSION)
	dkms install $(PACKAGE_NAME)/$(PACKAGE_VERSION) --force

dkms-uninstall:
	-dkms remove $(PACKAGE_NAME)/$(PACKAGE_VERSION) --all
	@if [ -L $(DKMS_SRC) ]; then \
		rm -f $(DKMS_SRC); \
	fi
