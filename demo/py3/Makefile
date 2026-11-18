ifndef TUXDIR
$(error TUXDIR is not set)
endif

export PATH:=$(TUXDIR)/bin:$(PATH)
export LD_LIBRARY_PATH:=$(TUXDIR)/lib:$(LD_LIBRARY_PATH)
export TUXCONFIG:=$(CURDIR)/t
export FLDTBLDIR32=$(TUXDIR)/udataobj:$(CURDIR)
# 22.1 moved SRVCNM and friends out of tpadm into Usysfl32
ifneq ($(wildcard $(TUXDIR)/udataobj/Usysfl32),)
export FIELDTBLS32=tpadm,Usysfl32,demo
else
export FIELDTBLS32=tpadm,demo
endif

default: t
	-tmunloadcf
	./buf.py < in.ud
	tmboot -y
	curl -X POST --data '{"USD": "5"}' localhost:8000
	@echo
	./client.py
	tmshutdown -y

buf:
	./buf.py < in.ud

ubbconfig: ubbconfig.in
	cat $< \
          | sed s:@TUXDIR@:$(TUXDIR):g \
          | sed s:@UNAME@:`uname -n`:g \
          | sed s:@CURDIR@:$(CURDIR):g > $@

t: ubbconfig
	tmloadcf -y $<

clean:
	-tmipcrm -y
	-rm -rf *.o ubbconfig t ULOG.* access.* stdout stderr .adm
