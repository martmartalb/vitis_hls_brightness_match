RM = rm -rf
VITIS = $(XILINX_VITIS)/bin/vitis

# Project config
HLS_COMPONENT_NAME = brightness_match
HLS_CFG_FILE       = config/hls_config.cfg
VITIS_WS           = vitis_workspace

# Scripts
BUILD_SCRIPT = scripts/vitis_build_comands.py

# Default target
.PHONY: all
all: synth

# Create HLS component project
.PHONY: hls_component
hls_component: $(VITIS_WS)/$(HLS_COMPONENT_NAME)/hls_config.cfg

$(VITIS_WS)/$(HLS_COMPONENT_NAME)/hls_config.cfg: $(HLS_CFG_FILE) $(BUILD_SCRIPT)
	@echo "==> Creating HLS component project..."
	$(VITIS) -s $(BUILD_SCRIPT) create_hls_component_project \
		--workspace $(VITIS_WS) \
		--name $(HLS_COMPONENT_NAME) \
		--cfg_file $(HLS_CFG_FILE)

# C Simulation
.PHONY: csim
csim: hls_component
	@echo "==> Running C Simulation..."
	$(VITIS) -s $(BUILD_SCRIPT) run_csim \
		--workspace $(VITIS_WS) \
		--name $(HLS_COMPONENT_NAME)

# Synthesis
.PHONY: synth
synth: hls_component
	@echo "==> Running Synthesis..."
	$(VITIS) -s $(BUILD_SCRIPT) run_synth \
		--workspace $(VITIS_WS) \
		--name $(HLS_COMPONENT_NAME)

# Co-Simulation
.PHONY: cosim
cosim: hls_component
	@echo "==> Running Co-Simulation..."
	$(VITIS) -s $(BUILD_SCRIPT) run_cosim \
		--workspace $(VITIS_WS) \
		--name $(HLS_COMPONENT_NAME)

# Clean everything
.PHONY: clean
clean:
	$(RM) $(VITIS_WS) vivado* .Xil *.log *.jou
