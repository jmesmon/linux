always := gdb-scripts

$(obj)/gdb-scripts:
ifneq ($(KBUILD_SRC),)
	$(Q)ln -fsn $(srctree)/$(obj)/*.py $(objtree)/$(obj)
endif
	@:

clean-files := *.pyc $(if $(KBUILD_SRC),*.py)
