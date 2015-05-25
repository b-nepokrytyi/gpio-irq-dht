#
# Copyright (C) 2008-2012 OpenWrt.org
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=gpio-irq-dht
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define KernelPackage/gpio-irq-dht
  SUBMENU:=Other modules
  DEPENDS:=@!LINUX_3_3
  TITLE:=GPIO IRQ DHT22 reader
  FILES:=$(PKG_BUILD_DIR)/gpio-irq-dht.ko
  AUTOLOAD:=$(call AutoLoad,30,gpio-irq-dht,1)
  KCONFIG:=
endef

define KernelPackage/gpio-irq-dht/description
 This is a DHT22 sensor kernel module for AR9331 devices.
endef

MAKE_OPTS:= \
	ARCH="$(LINUX_KARCH)" \
	CROSS_COMPILE="$(TARGET_CROSS)" \
	SUBDIRS="$(PKG_BUILD_DIR)"

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(MAKE) -C "$(LINUX_DIR)" \
		$(MAKE_OPTS) \
		modules
endef

$(eval $(call KernelPackage,gpio-irq-dht))
