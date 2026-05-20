# SPDX-License-Identifier: GPL-2.0

obj-$(CONFIG_DRM_ACCEL_APOLLO_HEXAGON) += apollo-hexagon-drm.o

apollo-hexagon-drm-y := apollo-hexagon.o apollo-hexagon-bo.o \
	apollo-hexagon-context.o apollo-hexagon-compat.o \
	apollo-hexagon-exec.o apollo-hexagon-fault.o \
	apollo-hexagon-fence.o apollo-hexagon-iommu.o
apollo-hexagon-drm-$(CONFIG_DRM_ACCEL_APOLLO_HEXAGON_SELFTEST) += \
	apollo-hexagon-selftest.o
