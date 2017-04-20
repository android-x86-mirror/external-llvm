LOCAL_PATH:= $(call my-dir)

# For the host
# =====================================================
include $(CLEAR_VARS)

LOCAL_SRC_FILES :=	\
	MCJIT.cpp \
	SectionMemoryManager.cpp

LOCAL_MODULE:= libLLVMMCJIT

LOCAL_MODULE_TAGS := optional

include $(LLVM_HOST_BUILD_MK)
include $(BUILD_HOST_STATIC_LIBRARY)

# For the device
# =====================================================
include $(CLEAR_VARS)

LOCAL_SRC_FILES :=	\
	MCJIT.cpp \
	SectionMemoryManager.cpp

LOCAL_MODULE:= libLLVMMCJIT

LOCAL_MODULE_TAGS := optional

include $(LLVM_DEVICE_BUILD_MK)
include $(BUILD_STATIC_LIBRARY)
