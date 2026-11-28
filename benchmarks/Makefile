DATA_DIR = data

SCHBENCH_DIR = schbench
SCHBENCH_EXEC = $(SCHBENCH_DIR)/schbench

RT_TESTS_DIR = rt-tests
HACKBENCH_EXEC = $(RT_TESTS_DIR)/hackbench

SYSBENCH_DIR = sysbench
SYSBENCH_EXEC = $(SYSBENCH_DIR)/src/sysbench

FIBO_DIR = fibo
FIBO_EXEC = $(FIBO_DIR)/fibo

CPUSTRESS_DIR = cpustress
CPUSTRESS_EXEC = $(CPUSTRESS_DIR)/cpustress

SYNTHETIC_DIR = synthetic
SYNTHETIC_EXEC = $(SYNTHETIC_DIR)/synthetic

all: $(SCHBENCH_EXEC) $(HACKBENCH_EXEC) $(SYSBENCH_EXEC) $(FIBO_EXEC) $(CPUSTRESS_EXEC) $(SYNTHETIC_EXEC) $(DATA_DIR)

$(SCHBENCH_EXEC): 
	make -C $(SCHBENCH_DIR)

$(HACKBENCH_EXEC):
	make -C $(RT_TESTS_DIR) hackbench

$(SYSBENCH_EXEC):
	cd $(SYSBENCH_DIR) && \
	./autogen.sh && \
	./configure
	make -C $(SYSBENCH_DIR) -j

$(FIBO_EXEC):
	make -C $(FIBO_DIR)

$(CPUSTRESS_EXEC):
	make -C $(CPUSTRESS_DIR)

$(SYNTHETIC_EXEC):
	make -C $(SYNTHETIC_DIR)

$(DATA_DIR):
	mkdir $@

clean:
	make -C $(FIBO_DIR) clean
	make -C $(CPUSTRESS_DIR) clean
	make -C $(SYNTHETIC_DIR) clean

.PHONY: all clean