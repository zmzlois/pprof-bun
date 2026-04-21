## builds script/*.node addons for JSC symbol exploration
## requires: node-addon-api (bun add node-addon-api), clang++, node headers

BUN      := $(shell which bun)
NODE     := $(shell which node)
NODE_INC := $(shell $(NODE) -e "console.log(require('path').join(process.execPath,'../../include/node'))")
NAPI_INC := $(shell $(NODE) -e "console.log(require('node-addon-api').include_dir)")

CXXFLAGS := \
  -std=c++17 \
  -O2 \
  -fPIC \
  -I$(NODE_INC) \
  -I$(NAPI_INC) \
  -DNAPI_VERSION=9 \
  -DNAPI_DISABLE_CPP_EXCEPTIONS

PROBE_LAZY   := script/probe_lazy.node
PROBE_DIRECT := script/probe_direct.node
SLIDE        := script/slide.node

.PHONY: all clean

all: $(PROBE_LAZY) $(PROBE_DIRECT) $(SLIDE)

# probe — strategy 1: defer all unresolved symbols to the host at load time
$(PROBE_LAZY): script/probe.cc
	clang++ -bundle -undefined dynamic_lookup \
	  $(CXXFLAGS) -o $@ $<
	@echo "built $@"

# probe — strategy 2: validate undefined symbols against bun at link time
$(PROBE_DIRECT): script/probe.cc
	clang++ -bundle -bundle_loader $(BUN) \
	  $(CXXFLAGS) -o $@ $<
	@echo "built $@"

# slide — resolves binary offsets to runtime VAs using the ASLR slide
$(SLIDE): script/slide.cc
	clang++ -bundle -undefined dynamic_lookup \
	  $(CXXFLAGS) -o $@ $<
	@echo "built $@"

clean:
	rm -f $(PROBE_LAZY) $(PROBE_DIRECT) $(SLIDE)
