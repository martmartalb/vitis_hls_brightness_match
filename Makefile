RM = rm -rf
VIVADO = $(XILINX_VIVADO)/bin/vivado
VITIS = $(XILINX_VITIS)/bin/vitis

# Project config
VIVADO_PROJECT_NAME = vivado_project
VIVADO_PROJECT_DIR  = $(VIVADO_PROJECT_NAME)
PLATFORM_NAME = platform

VITIS_APP_NAME = hdmi_demo_app
VITIS_WS = vitis_workspace

# Scripts
CREATE_PROJECT_SCRIPT   = scripts/create_project.tcl
GEN_HW_PLATFORM_SCRIPT   = scripts/gen_hw_platform.tcl
GEN_VITIS_PLATFORM_SCRIPT = scripts/create_vitis_project.py

# Default: no trace
TRACE_FLAG = -notrace

# If "trace" is passed as a target → remove -notrace
ifeq ($(filter trace,$(MAKECMDGOALS)),trace)
TRACE_FLAG =
endif

# Default target
.PHONY: all
all: vitis_app

# Create Vivado project
.PHONY: vivado_project
vivado_project: $(VIVADO_PROJECT_DIR)/$(VIVADO_PROJECT_NAME).xpr


$(VIVADO_PROJECT_DIR)/$(VIVADO_PROJECT_NAME).xpr: $(CREATE_PROJECT_SCRIPT)
	@echo "==> Creating Vivado project..."
	$(VIVADO) -mode batch $(TRACE_FLAG) -source $(CREATE_PROJECT_SCRIPT) \
		-tclargs --origin_dir $(shell pwd)

# Create HW Platform
.PHONY: hw_platform
hw_platform: $(VIVADO_PROJECT_DIR)/$(PLATFORM_NAME).xsa

$(VIVADO_PROJECT_DIR)/$(PLATFORM_NAME).xsa: $(VIVADO_PROJECT_DIR)/$(VIVADO_PROJECT_NAME).xpr $(GEN_HW_PLATFORM_SCRIPT)
	@echo "==> Generating HW Platform..."
	$(VIVADO) -mode batch $(TRACE_FLAG) -source $(GEN_HW_PLATFORM_SCRIPT) \
		-tclargs --origin_dir $(shell pwd) --project_name $(VIVADO_PROJECT_DIR) \
		--platform_name $(PLATFORM_NAME)

# Create Vitis platform
.PHONY: vitis_platform
vitis_platform: $(VITIS_WS)/$(PLATFORM_NAME)/export/$(PLATFORM_NAME)/$(PLATFORM_NAME).xpfm

$(VITIS_WS)/$(PLATFORM_NAME)/export/$(PLATFORM_NAME)/$(PLATFORM_NAME).xpfm: $(VIVADO_PROJECT_DIR)/$(PLATFORM_NAME).xsa $(GEN_VITIS_PLATFORM_SCRIPT)
	@echo "==> Creating Vitis platform..."
	$(VITIS) -s $(GEN_VITIS_PLATFORM_SCRIPT) platform \
		--xsa $(VIVADO_PROJECT_DIR)/$(PLATFORM_NAME).xsa \
		--workspace $(VITIS_WS) \
		--name $(PLATFORM_NAME)

# Create Vitis app
.PHONY: vitis_app
vitis_app: $(VITIS_WS)/$(VITIS_APP_NAME)/.created

$(VITIS_WS)/$(VITIS_APP_NAME)/.created: $(VITIS_WS)/$(PLATFORM_NAME)/export/$(PLATFORM_NAME)/$(PLATFORM_NAME).xpfm $(GEN_VITIS_PLATFORM_SCRIPT)
	@echo "==> Creating Vitis app..."
	$(VITIS) -s $(GEN_VITIS_PLATFORM_SCRIPT) vitis_app \
		--workspace $(VITIS_WS) \
		--xpfm $(VITIS_WS)/$(PLATFORM_NAME)/export/$(PLATFORM_NAME)/$(PLATFORM_NAME).xpfm \
		--name $(VITIS_APP_NAME) \
		--src_dir src/$(VITIS_APP_NAME)/src

# Clean everything
.PHONY: clean
clean:
	$(RM) $(VIVADO_PROJECT_DIR) $(VITIS_WS) vivado* .Xil *.log *.jou