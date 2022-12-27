ifndef TUXDIR
$(error TUXDIR is not set)
endif

export PATH:=$(TUXDIR)/bin:$(PATH)
export LD_LIBRARY_PATH:=$(TUXDIR)/lib:$(LD_LIBRARY_PATH)
export TUXCONFIG:=$(CURDIR)/tuxconfig
export FLDTBLDIR32=$(TUXDIR)/udataobj:$(CURDIR)
export FIELDTBLS32=tpadm,demo

default: tuxconfig
	./buf.py < in.ud
	tmboot -y
	curl -X POST --data '{"USD": "5"}' localhost:8000
	@echo
	./client.py
	tmshutdown -y

py2: tuxconfig
	sed -i 's/python3/python/g' *py
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

tuxconfig: ubbconfig
	tmloadcf -y $<

clean:
	-tmipcrm -y
	-rm -rf *.o ubbconfig tuxconfig ULOG.* access.* stdout stderr .adm
