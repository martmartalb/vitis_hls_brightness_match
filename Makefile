RM = rm -rf
VITIS = $(XILINX_VITIS)/bin/vitis

# Project config
HLS_COMPONENT_NAME = brightness_match
HLS_CFG_FILE       = src/hls_config.cfg
HLS_SYN_FILE       = src/ip/brightness_match.cpp
HLS_TB_FILE        = src/tb/brightness_match_tb.cpp
VITIS_WS           = vitis_workspace

# Scripts
BUILD_SCRIPT = scripts/vitis_build_comands.py

# Default target
.PHONY: all
all: synth

# Create HLS component project
.PHONY: create_project
create_project: $(VITIS_WS)/$(HLS_COMPONENT_NAME)/hls_config.cfg

$(VITIS_WS)/$(HLS_COMPONENT_NAME)/hls_config.cfg: $(HLS_CFG_FILE) $(HLS_SYN_FILE) $(HLS_TB_FILE) $(BUILD_SCRIPT)
	@echo "==> Creating HLS component project..."
	$(VITIS) -s $(BUILD_SCRIPT) create_hls_component_project \
		--workspace $(VITIS_WS) \
		--name $(HLS_COMPONENT_NAME) \
		--cfg_file $(HLS_CFG_FILE) \
		--syn_file $(HLS_SYN_FILE) \
		--tb_file $(HLS_TB_FILE)

# C Simulation
.PHONY: csim
csim: create_project
	@echo "==> Running C Simulation..."
	$(VITIS) -s $(BUILD_SCRIPT) run_csim \
		--workspace $(VITIS_WS) \
		--name $(HLS_COMPONENT_NAME)

# Synthesis
.PHONY: synth
synth: create_project
	@echo "==> Running Synthesis..."
	$(VITIS) -s $(BUILD_SCRIPT) run_synth \
		--workspace $(VITIS_WS) \
		--name $(HLS_COMPONENT_NAME)

# Co-Simulation
.PHONY: cosim
cosim: create_project
	@echo "==> Running Co-Simulation..."
	$(VITIS) -s $(BUILD_SCRIPT) run_cosim \
		--workspace $(VITIS_WS) \
		--name $(HLS_COMPONENT_NAME)

# Clean everything
.PHONY: clean
clean:
	$(RM) $(VITIS_WS) vivado* .Xil *.log *.jou
