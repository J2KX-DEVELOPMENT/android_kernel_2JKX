#
# Makefile for the exynos video drivers.
#
KBUILD_CFLAGS += -fdata-sections -ffunction-sections -finline-functions -ftree-vectorize -funroll-loops -ffast-math -fforce-addr -Wno-unused-variable -Wno-unused-function

obj-$(CONFIG_EXYNOS_MIPI_DSI)		+= exynos_mipi_dsi.o exynos_mipi_dsi_common.o \
				     	exynos_mipi_dsi_lowlevel.o
obj-$(CONFIG_EXYNOS_LCD_S6E8AX0)	+= s6e8ax0.o
obj-$(CONFIG_EXYNOS_DECON_7570)	+= decon_7570/
